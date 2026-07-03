/*
 * claude.c - Claude on the Amiga (RustChain Amiga Edition, Phase 5)
 *
 * A native C client for the Anthropic Messages API, for classic AmigaOS
 * (m68k) and AROS. This is "Claude Code, non-Node" for the Amiga: a small
 * one-shot / REPL chat client plus a real tool-use loop that lets Claude
 * read and write files and run commands on the Amiga's own filesystem.
 *
 * Two transports:
 *   1. PRIMARY  - AmiSSL direct HTTPS to api.anthropic.com:443. Self
 *                 contained: no host proxy, the API key lives on the Amiga
 *                 (ENV:ANTHROPIC_API_KEY or SYS:.claude/config). Needs a
 *                 full AmigaOS/AROS with AmiSSL + bsdsocket installed.
 *   2. FALLBACK - plain HTTP POST to a host-side proxy (claude --proxy
 *                 host:port). For machines with no TLS library (bare-ROM
 *                 AROS). The proxy holds the key host-side; the Amiga sends
 *                 no key in this mode.
 *
 * API facts (Anthropic Messages API, do not deviate):
 *   POST https://api.anthropic.com/v1/messages
 *   headers: x-api-key, anthropic-version: 2023-06-01, content-type json
 *   body: {"model","max_tokens","messages":[...],"tools":[...]}
 *   reply: {"content":[{"type":"text","text":...}|{"type":"tool_use",...}],
 *           "stop_reason":"end_turn"|"tool_use"|...}
 *   tool loop: on stop_reason "tool_use" reply with a user message of
 *   tool_result blocks, loop until "end_turn".
 *
 * m68k rules (carried from miner/README.md and tools/common/rtc_common.c):
 *   - C89-friendly: declarations first, no // comments, no C99 loop decls.
 *   - No 64-bit shifts, no %lld, ILP32, big endian.
 *   - Buffers are static/global to keep the stack small (TLS is stack-hungry).
 *
 * Vendored helpers: vendor/rtc_common.c (copied from tools/common), used for
 * the JSON object scan, HTTP header parse, and bsdsocket break/sleep helpers.
 * The AmiSSL init sequence follows the SDK example https.c precisely.
 *
 * Build:
 *   Amiga hunk exe : see Makefile target `claude` (docker cross + AmiSSL)
 *   Host self-test : -DHOST_TEST (see Makefile target `host-test`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/rtc_common.h"

/* -------- sizes (static buffers; target is full AmigaOS/AROS w/ fast RAM) -- */
#define API_HOST      "api.anthropic.com"
#define API_PORT      443
#define API_PATH      "/v1/messages"
#define ANTHRO_VER    "2023-06-01"
#define DEF_MODEL     "claude-opus-4-8"
#define DEF_MAXTOK    4096L
#define MAX_ITERS     8
#define MAX_TOOLS     8
#define MAX_MSGS      48

#define RESPBUF       65536
#define JOINBUF       90000
#define REQBODY       (JOINBUF + 4096)
#define CONTENTBUF    49152
#define TEXTBUF       32768
#define OBJBUF        40960
#define INPUTBUF      2048

/* Buffers are sized so every sprintf destination is provably larger than the
   most its escaped source can produce (keeps -Wformat-overflow quiet and the
   construction safe): a prompt escapes to at most 2x its cap, a tool result's
   content to at most 2x TOOL_OUT_MAX. */
#define PROMPT_ESC    8192                 /* escaped prompt cap */
#define MSGOBJ_BUF    (PROMPT_ESC + 128)   /* a role/text message object */
#define TOOL_OUT_MAX  16000                /* max tool output bytes we return */
#define TOOL_ESC      (TOOL_OUT_MAX * 2 + 32)
#define TRBLOCK_BUF   (TOOL_ESC + 128)     /* one tool_result block */

/* ================================================================== */
/* Pure JSON helpers (compiled for host self-test AND Amiga)          */
/* ================================================================== */

/* JSON-escape src into dst. Returns length, or -1 if it would overflow. */
static int json_escape(char *dst, int dstlen, const char *src)
{
    static const char hx[] = "0123456789abcdef";
    const unsigned char *s = (const unsigned char *)src;
    int o = 0;

    for (; *s; s++) {
        unsigned char c = *s;
        if (o >= dstlen - 7) {
            dst[o] = '\0';
            return -1;
        }
        if (c == '"') { dst[o++] = '\\'; dst[o++] = '"'; }
        else if (c == '\\') { dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
        else if (c < 0x20) {
            dst[o++] = '\\'; dst[o++] = 'u'; dst[o++] = '0'; dst[o++] = '0';
            dst[o++] = hx[(c >> 4) & 0x0F]; dst[o++] = hx[c & 0x0F];
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

/* Decode a JSON string body (the chars between the quotes) into dst.
   Handles \n \r \t \b \f \/ \" \\ and \uXXXX (encoded to UTF-8). */
static int json_unescape(const char *src, int srclen, char *dst, int dstlen)
{
    int i = 0, o = 0;

    while (i < srclen && o < dstlen - 4) {
        char c = src[i];
        if (c == '\\' && i + 1 < srclen) {
            char n = src[i + 1];
            if (n == 'n') { dst[o++] = '\n'; i += 2; }
            else if (n == 'r') { dst[o++] = '\r'; i += 2; }
            else if (n == 't') { dst[o++] = '\t'; i += 2; }
            else if (n == 'b') { dst[o++] = '\b'; i += 2; }
            else if (n == 'f') { dst[o++] = '\f'; i += 2; }
            else if (n == '/') { dst[o++] = '/'; i += 2; }
            else if (n == '"') { dst[o++] = '"'; i += 2; }
            else if (n == '\\') { dst[o++] = '\\'; i += 2; }
            else if (n == 'u' && i + 5 < srclen) {
                int v = 0, k;
                for (k = 0; k < 4; k++) {
                    char h = src[i + 2 + k];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= (h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (h - 'A' + 10);
                }
                if (v < 0x80) {
                    dst[o++] = (char)v;
                } else if (v < 0x800) {
                    dst[o++] = (char)(0xC0 | (v >> 6));
                    dst[o++] = (char)(0x80 | (v & 0x3F));
                } else {
                    dst[o++] = (char)(0xE0 | (v >> 12));
                    dst[o++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (v & 0x3F));
                }
                i += 6;
            } else {
                dst[o++] = c; i++;
            }
        } else {
            dst[o++] = c; i++;
        }
    }
    dst[o] = '\0';
    return o;
}

/* Find "key" used as an object key (i.e. followed by a colon) and return a
   pointer just past that colon. Skips occurrences where the same text is a
   string value rather than a key, so "type":"text" does not shadow the real
   "text" key. Returns NULL if the key is not present. */
static const char *find_key(const char *json, const char *key)
{
    char pat[80];
    const char *p;
    int kl;

    if ((int)strlen(key) > 70) return NULL;
    sprintf(pat, "\"%s\"", key);
    kl = (int)strlen(pat);
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + kl;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q == ':')
            return q + 1;
        p += kl;
    }
    return NULL;
}

/* Find "key":"value" and unescape value into out. Returns 1 on success. */
static int get_json_string(const char *json, const char *key,
                           char *out, int outlen)
{
    const char *p, *vs;
    int vlen, instr;

    if (outlen < 1) return 0;
    out[0] = '\0';
    p = find_key(json, key);
    if (!p) return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '"') return 0;
    p++;
    vs = p;
    instr = 1;
    while (*p && instr) {
        if (*p == '\\' && p[1]) p += 2;
        else if (*p == '"') { instr = 0; break; }
        else p++;
    }
    vlen = (int)(p - vs);
    json_unescape(vs, vlen, out, outlen);
    return 1;
}

/* Find "key": followed by a balanced [..] or {..} and hand back the raw
   span (start pointer + length), string-aware. Returns 1 on success. */
static int get_raw_span(const char *json, const char *key,
                        const char **start, int *len)
{
    const char *p;
    char open, close;
    int depth = 0, instr = 0;

    p = find_key(json, key);
    if (!p) return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '[') { open = '['; close = ']'; }
    else if (*p == '{') { open = '{'; close = '}'; }
    else return 0;
    *start = p;
    for (; *p; p++) {
        char ch = *p;
        if (instr) {
            if (ch == '\\' && p[1]) p++;
            else if (ch == '"') instr = 0;
        } else {
            if (ch == '"') instr = 1;
            else if (ch == open) depth++;
            else if (ch == close) {
                depth--;
                if (depth == 0) {
                    *len = (int)(p - *start) + 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* a single tool_use block requested by the model */
struct tool_call {
    char id[128];
    char name[32];
    char input[INPUTBUF];   /* the raw JSON of the "input" object */
};

/* Walk a content array [...]. Concatenate every text block into textout and
   record every tool_use block into calls[]. */
static void collect_text_and_tools(const char *content_array,
                                   char *textout, int textlen,
                                   struct tool_call *calls, int *ncalls,
                                   int maxcalls)
{
    static char obj[OBJBUF];
    const char *p = content_array;
    int to = 0;

    *ncalls = 0;
    textout[0] = '\0';
    for (;;) {
        char type[24];
        p = rtc_json_next_obj(p, obj, sizeof(obj));
        if (!p) break;
        get_json_string(obj, "type", type, sizeof(type));
        if (strcmp(type, "text") == 0) {
            static char t[TEXTBUF];
            if (get_json_string(obj, "text", t, sizeof(t))) {
                int l = (int)strlen(t);
                if (to + l < textlen - 1) {
                    memcpy(textout + to, t, l);
                    to += l;
                }
            }
        } else if (strcmp(type, "tool_use") == 0 && *ncalls < maxcalls) {
            struct tool_call *c = &calls[*ncalls];
            const char *s;
            int len;
            c->id[0] = c->name[0] = c->input[0] = '\0';
            get_json_string(obj, "id", c->id, sizeof(c->id));
            get_json_string(obj, "name", c->name, sizeof(c->name));
            if (get_raw_span(obj, "input", &s, &len)) {
                if (len > (int)sizeof(c->input) - 1)
                    len = (int)sizeof(c->input) - 1;
                memcpy(c->input, s, len);
                c->input[len] = '\0';
            }
            (*ncalls)++;
        }
    }
    textout[to] = '\0';
}

/* Build one tool_result block. Returns length or -1. */
static int build_tool_result(char *dst, int dstlen, const char *id,
                             const char *content, int is_error)
{
    static char esc[TOOL_ESC];
    int n;

    if (json_escape(esc, sizeof(esc), content) < 0)
        return -1;
    n = sprintf(dst,
        "{\"type\":\"tool_result\",\"tool_use_id\":\"%s\",\"content\":\"%s\"%s}",
        id, esc, is_error ? ",\"is_error\":true" : "");
    if (n >= dstlen)
        return -1;
    return n;
}

/* Build a {"role":"user","content":"<text>"} message. Returns length or -1. */
static int build_user_text_message(char *dst, int dstlen, const char *text)
{
    static char esc[PROMPT_ESC];
    int n;

    if (json_escape(esc, sizeof(esc), text) < 0)
        return -1;
    n = sprintf(dst, "{\"role\":\"user\",\"content\":\"%s\"}", esc);
    if (n >= dstlen)
        return -1;
    return n;
}

/* the fixed tool catalogue offered to the model */
static const char *TOOLS_JSON =
"[{\"name\":\"read_file\",\"description\":\"Read a text file from the Amiga "
"filesystem. Give an AmigaDOS path like SYS:S/startup-sequence.\","
"\"input_schema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
"\"string\"}},\"required\":[\"path\"]}},"
"{\"name\":\"write_file\",\"description\":\"Write text to a file on the Amiga "
"filesystem, overwriting it. Give path and text.\",\"input_schema\":{\"type\":"
"\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"text\":{\"type\":"
"\"string\"}},\"required\":[\"path\",\"text\"]}},"
"{\"name\":\"run_command\",\"description\":\"Run an AmigaDOS shell command and "
"return its output. Give cmd.\",\"input_schema\":{\"type\":\"object\","
"\"properties\":{\"cmd\":{\"type\":\"string\"}},\"required\":[\"cmd\"]}}]";

/* Build the request body from the joined message array. Returns length/-1. */
static int build_request(char *dst, int dstlen, const char *model,
                         long maxtok, int with_tools,
                         const char *messages_joined)
{
    int n;

    if (with_tools) {
        n = sprintf(dst,
            "{\"model\":\"%s\",\"max_tokens\":%ld,\"tools\":%s,"
            "\"messages\":[%s]}",
            model, maxtok, TOOLS_JSON, messages_joined);
    } else {
        n = sprintf(dst,
            "{\"model\":\"%s\",\"max_tokens\":%ld,\"messages\":[%s]}",
            model, maxtok, messages_joined);
    }
    if (n >= dstlen)
        return -1;
    return n;
}

/* ================================================================== */
/* Everything below is platform code (real on Amiga, stubbed on host) */
/* ================================================================== */

#ifndef HOST_TEST

/* -------- config (set from argv / environment) -------- */
static const char *g_model      = DEF_MODEL;
static long        g_maxtok     = DEF_MAXTOK;
static int         g_use_proxy  = 0;
static char        g_proxy_host[128] = "127.0.0.1";
static int         g_proxy_port = 8790;
static int         g_yes        = 0;   /* auto-approve destructive tools */
static int         g_insecure   = 0;   /* skip TLS verify (AmiSSL mode) */
static int         g_use_tools  = 1;
static int         g_verbose    = 0;   /* print transport progress to stdout */
static char        g_api_key[256];

/* conversation history: each entry is one complete JSON message object */
static char       *g_hist[MAX_MSGS];
static int         g_nhist = 0;

/* ---- history management ---- */

static void hist_add(const char *obj)
{
    char *copy;

    if (g_nhist >= MAX_MSGS) {
        /* drop the two oldest (a user/assistant pair) to bound memory */
        free(g_hist[0]);
        free(g_hist[1]);
        memmove(&g_hist[0], &g_hist[2], (g_nhist - 2) * sizeof(char *));
        g_nhist -= 2;
    }
    copy = (char *)malloc(strlen(obj) + 1);
    if (!copy)
        return;
    strcpy(copy, obj);
    g_hist[g_nhist++] = copy;
}

/* Join the history into out with commas. Drops the oldest entries if it does
   not fit. Returns the number of characters written. */
static int hist_join(char *out, int outlen)
{
    int start = 0;
    int total, o, i;

    for (;;) {
        total = 0;
        for (i = start; i < g_nhist; i++)
            total += (int)strlen(g_hist[i]) + 1;
        if (total < outlen - 1 || start >= g_nhist - 1)
            break;
        start++;
    }
    o = 0;
    for (i = start; i < g_nhist; i++) {
        int l = (int)strlen(g_hist[i]);
        if (o + l + 2 >= outlen)
            break;
        if (i > start)
            out[o++] = ',';
        memcpy(out + o, g_hist[i], l);
        o += l;
    }
    out[o] = '\0';
    return o;
}

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

/* larger stack: TLS handshake is stack hungry */
const char stack_size[] = "$STACK:131072";

/* SocketBase is owned by the vendored rtc_common.c; share the one instance */
extern struct Library *SocketBase;

struct Library *AmiSSLMasterBase = NULL;    /* proto/amisslmaster.h expects it */
struct Library *AmiSSLBase = NULL;          /* proto/amissl.h expects this name */
struct Library *UtilityBase = NULL;         /* opened for tag handling */
static int g_amissl_errno = 0;
static int g_amissl_ready = 0;

/* ---- small helpers ---- */

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int i;
    char *end;

    for (i = 0; i < 4; i++) {
        parts[i] = strtoul(s, &end, 10);
        if (end == s || parts[i] > 255) return 0;
        if (i < 3 && *end != '.') return 0;
        s = end + 1;
    }
    if (*end != '\0') return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static long tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    unsigned long ip;
    long s;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (unsigned short)port;  /* m68k big endian: htons is identity */

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            fprintf(stderr, "[claude] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "[claude] socket() failed\n");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[claude] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* ---- plain-HTTP proxy transport (fallback) ---- */

static long proxy_post(const char *host, int port, const char *body,
                       char *resp, long resplen)
{
    static char req[REQBODY + 512];
    long s, got, total = 0;
    int hdr, blen;

    if (!rtc_net_open())
        return -1;
    blen = (int)strlen(body);
    hdr = sprintf(req,
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: claude-amiga/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        host, port, blen);
    if (hdr < 0 || hdr + blen >= (int)sizeof(req)) {
        fprintf(stderr, "[claude] request too large for proxy buffer\n");
        return -1;
    }
    memcpy(req + hdr, body, blen);
    hdr += blen;

    if (g_verbose)
        printf("[claude] proxy connecting to %s:%d\n", host, port);
    s = tcp_connect(host, port);
    if (s < 0)
        return -1;
    if (g_verbose)
        printf("[claude] connected, sending %d bytes\n", hdr);
    if (send(s, req, hdr, 0) != hdr) {
        fprintf(stderr, "[claude] proxy send failed\n");
        CloseSocket(s);
        return -1;
    }
    if (g_verbose)
        printf("[claude] request sent, waiting for reply\n");
    while (total < resplen - 1) {
        got = recv(s, resp + total, resplen - 1 - total, 0);
        if (got <= 0)
            break;
        total += got;
    }
    resp[total] = '\0';
    CloseSocket(s);
    if (g_verbose)
        printf("[claude] got %ld bytes\n", total);
    return total;
}

/* ---- AmiSSL init (follows SDK example https.c, OS3/m68k path) ---- */

static void amissl_cleanup(void)
{
    if (g_amissl_ready) {
        CleanupAmiSSLA(NULL);
        g_amissl_ready = 0;
    }
    if (AmiSSLBase) {
        CloseAmiSSL();
        AmiSSLBase = NULL;
    }
    if (AmiSSLMasterBase) {
        CloseLibrary(AmiSSLMasterBase);
        AmiSSLMasterBase = NULL;
    }
    if (UtilityBase) {
        CloseLibrary(UtilityBase);
        UtilityBase = NULL;
    }
}

static int amissl_init(void)
{
    if (g_amissl_ready)
        return 1;

    if (!rtc_net_open())          /* opens the shared SocketBase (v4) */
        return 0;

    UtilityBase = OpenLibrary((STRPTR)"utility.library", 0);
    if (!UtilityBase) {
        fprintf(stderr, "[claude] cannot open utility.library\n");
        return 0;
    }
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(stderr, "[claude] cannot open amisslmaster.library v%d\n"
                        "        install AmiSSL, or use --proxy host:port\n",
                        AMISSLMASTER_MIN_VERSION);
        amissl_cleanup();
        return 0;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        fprintf(stderr, "[claude] AmiSSL version is too old\n");
        amissl_cleanup();
        return 0;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        fprintf(stderr, "[claude] cannot open amissl.library\n");
        amissl_cleanup();
        return 0;
    }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&g_amissl_errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        fprintf(stderr, "[claude] InitAmiSSL failed\n");
        amissl_cleanup();
        return 0;
    }
    g_amissl_ready = 1;

    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT
                     | OPENSSL_INIT_ADD_ALL_CIPHERS
                     | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
    return 1;
}

static void seed_rand(void)
{
    unsigned char buf[64];
    struct DateStamp ds;
    int i;

    DateStamp(&ds);
    for (i = 0; i < (int)sizeof(buf); i++) {
        unsigned long m = (unsigned long)ds.ds_Tick + ds.ds_Minute + i * 2654435761UL;
        buf[i] = (unsigned char)(m ^ (m >> 8) ^ (m >> 16));
    }
    RAND_seed(buf, sizeof(buf));
}

/* ---- AmiSSL direct HTTPS transport (primary) ---- */

static long amissl_post(const char *body, char *resp, long resplen)
{
    static char req[REQBODY + 512];
    SSL_CTX *ctx;
    SSL *ssl;
    long sock, got, total = 0;
    int hdr, blen, rc;

    if (g_api_key[0] == '\0') {
        fprintf(stderr, "[claude] no API key. Set ENV:ANTHROPIC_API_KEY or\n"
                        "        SYS:.claude/config, or use --proxy host:port\n");
        return -1;
    }
    if (!amissl_init())
        return -1;
    seed_rand();

    blen = (int)strlen(body);
    hdr = sprintf(req,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "anthropic-version: %s\r\n"
        "User-Agent: claude-amiga/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        API_PATH, API_HOST, g_api_key, ANTHRO_VER, blen);
    if (hdr < 0 || hdr + blen >= (int)sizeof(req)) {
        fprintf(stderr, "[claude] request too large for TLS buffer\n");
        return -1;
    }
    memcpy(req + hdr, body, blen);
    hdr += blen;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fprintf(stderr, "[claude] SSL_CTX_new failed\n");
        return -1;
    }
    if (g_insecure) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    }

    ssl = SSL_new(ctx);
    if (!ssl) {
        fprintf(stderr, "[claude] SSL_new failed\n");
        SSL_CTX_free(ctx);
        return -1;
    }

    sock = tcp_connect(API_HOST, API_PORT);
    if (sock < 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, API_HOST);

    rc = SSL_connect(ssl);
    if (rc <= 0) {
        fprintf(stderr, "[claude] TLS handshake to %s failed", API_HOST);
        if (!g_insecure) {
            long vr = SSL_get_verify_result(ssl);
            if (vr != X509_V_OK)
                fprintf(stderr, " (cert verify: %s; try --insecure or install "
                                "the AmiSSL CA bundle)",
                        X509_verify_cert_error_string(vr));
        }
        fprintf(stderr, "\n");
        CloseSocket(sock);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }

    if (SSL_write(ssl, req, hdr) <= 0) {
        fprintf(stderr, "[claude] SSL_write failed\n");
        SSL_shutdown(ssl);
        CloseSocket(sock);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }

    while (total < resplen - 1) {
        got = SSL_read(ssl, resp + total, (int)(resplen - 1 - total));
        if (got <= 0)
            break;
        total += got;
        if (rtc_check_break())
            break;
    }
    resp[total] = '\0';

    SSL_shutdown(ssl);
    CloseSocket(sock);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return total;
}

/* ---- de-chunk an HTTP/1.1 chunked body in place (Amiga side) ----
   Some HTTP/1.1 responses arrive Transfer-Encoding: chunked. Rewrites the
   body region to the decoded bytes and NUL terminates. Safe no-op when the
   body is not chunked (caller only calls this when the header says so). */
static void dechunk_in_place(char *body)
{
    char *r = body, *w = body;

    for (;;) {
        long sz = strtol(r, NULL, 16);
        char *nl = strchr(r, '\n');
        if (!nl || sz <= 0)
            break;
        r = nl + 1;
        memmove(w, r, sz);
        w += sz;
        r += sz;
        while (*r == '\r' || *r == '\n')
            r++;
    }
    *w = '\0';
}

static int header_has_chunked(const char *resp, const char *body)
{
    const char *p = resp;
    while (p && p < body) {
        const char *nl = strchr(p, '\n');
        if (!nl || nl >= body)
            break;
        if (strncmp(p, "Transfer-Encoding:", 18) == 0 &&
            strstr(p, "chunked") && strstr(p, "chunked") < nl)
            return 1;
        p = nl + 1;
    }
    return 0;
}

/* ---- API key loading (Amiga) ---- */

static void load_api_key(void)
{
    char *env;
    BPTR fh;

    g_api_key[0] = '\0';

    env = getenv("ANTHROPIC_API_KEY");
    if (env && env[0]) {
        strncpy(g_api_key, env, sizeof(g_api_key) - 1);
        g_api_key[sizeof(g_api_key) - 1] = '\0';
        return;
    }

    /* SYS:.claude/config, lines of KEY=VALUE */
    fh = Open((STRPTR)"SYS:.claude/config", MODE_OLDFILE);
    if (fh) {
        static char cfg[2048];
        long n = Read(fh, cfg, sizeof(cfg) - 1);
        Close(fh);
        if (n > 0) {
            char *k;
            cfg[n] = '\0';
            k = strstr(cfg, "ANTHROPIC_API_KEY");
            if (k) {
                k += strlen("ANTHROPIC_API_KEY");
                while (*k == ' ' || *k == '=' || *k == '\t')
                    k++;
                {
                    int o = 0;
                    while (*k && *k != '\r' && *k != '\n' && *k != ' ' &&
                           o < (int)sizeof(g_api_key) - 1)
                        g_api_key[o++] = *k++;
                    g_api_key[o] = '\0';
                }
            }
        }
    }
}

/* ---- local tool execution (Amiga filesystem) ---- */

static int tool_read_file(const char *path, char *out, int outlen)
{
    BPTR fh;
    long n;

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        sprintf(out, "ERROR: cannot open %s for reading", path);
        return 0;
    }
    n = Read(fh, out, outlen - 1);
    Close(fh);
    if (n < 0) {
        strcpy(out, "ERROR: read failed");
        return 0;
    }
    out[n] = '\0';
    return 1;
}

static int tool_write_file(const char *path, const char *text,
                           char *out, int outlen)
{
    BPTR fh;
    int len = (int)strlen(text);
    long w;

    (void)outlen;
    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        sprintf(out, "ERROR: cannot open %s for writing", path);
        return 0;
    }
    w = Write(fh, (APTR)text, len);
    Close(fh);
    if (w != len) {
        sprintf(out, "ERROR: wrote %ld of %d bytes to %s", w, len, path);
        return 0;
    }
    sprintf(out, "OK: wrote %d bytes to %s", len, path);
    return 1;
}

static int tool_run_command(const char *cmd, char *out, int outlen)
{
    BPTR outfh, rfh;
    long rc, n = 0;

    outfh = Open((STRPTR)"T:claude_cmd.out", MODE_NEWFILE);
    if (!outfh) {
        strcpy(out, "ERROR: cannot open T:claude_cmd.out");
        return 0;
    }
    rc = Execute((STRPTR)cmd, 0, outfh);
    Close(outfh);

    rfh = Open((STRPTR)"T:claude_cmd.out", MODE_OLDFILE);
    if (rfh) {
        n = Read(rfh, out, outlen - 1);
        Close(rfh);
        if (n < 0)
            n = 0;
    }
    out[n] = '\0';
    DeleteFile((STRPTR)"T:claude_cmd.out");
    if (rc == 0 && n == 0)
        sprintf(out, "(command '%s' produced no output; it may not have run)",
                cmd);
    return 1;
}

static int confirm(const char *action, const char *target)
{
    char line[16];

    if (g_yes)
        return 1;
    printf("Claude wants to %s: %s. Allow? (y/N) ", action, target);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return 0;
    return (line[0] == 'y' || line[0] == 'Y');
}

static void execute_tool(struct tool_call *c, char *out, int outlen,
                         int *is_err)
{
    *is_err = 0;
    if (strcmp(c->name, "read_file") == 0) {
        char path[512];
        if (!get_json_string(c->input, "path", path, sizeof(path))) {
            strcpy(out, "ERROR: missing path");
            *is_err = 1;
            return;
        }
        printf("[claude] read_file %s\n", path);
        if (!tool_read_file(path, out, outlen))
            *is_err = 1;
    } else if (strcmp(c->name, "write_file") == 0) {
        char path[512];
        static char text[TEXTBUF];
        if (!get_json_string(c->input, "path", path, sizeof(path))) {
            strcpy(out, "ERROR: missing path");
            *is_err = 1;
            return;
        }
        get_json_string(c->input, "text", text, sizeof(text));
        if (!confirm("write file", path)) {
            strcpy(out, "User denied the write_file operation.");
            *is_err = 1;
            return;
        }
        if (!tool_write_file(path, text, out, outlen))
            *is_err = 1;
    } else if (strcmp(c->name, "run_command") == 0) {
        char cmd[512];
        if (!get_json_string(c->input, "cmd", cmd, sizeof(cmd))) {
            strcpy(out, "ERROR: missing cmd");
            *is_err = 1;
            return;
        }
        if (!confirm("run command", cmd)) {
            strcpy(out, "User denied the run_command operation.");
            *is_err = 1;
            return;
        }
        if (!tool_run_command(cmd, out, outlen))
            *is_err = 1;
    } else {
        sprintf(out, "ERROR: unknown tool %s", c->name);
        *is_err = 1;
    }
}

/* ---- the conversation / tool-use loop ---- */

static long transport_send(const char *body, char *resp, long resplen)
{
    if (g_use_proxy)
        return proxy_post(g_proxy_host, g_proxy_port, body, resp, resplen);
    return amissl_post(body, resp, resplen);
}

/* Run one exchange to completion (looping through any tool use). The initial
   user message must already be on the history. Returns 1 ok, 0 on error. */
static int run_conversation(void)
{
    static char joined[JOINBUF];
    static char reqbody[REQBODY];
    static char resp[RESPBUF];
    static char content_arr[CONTENTBUF];
    static char textout[TEXTBUF];
    static char toolmsg[TRBLOCK_BUF + 4096];
    static char asst[CONTENTBUF + 128];
    struct tool_call calls[MAX_TOOLS];
    int iter;

    for (iter = 0; iter < MAX_ITERS; iter++) {
        const char *body, *cs;
        int status, clen, ncalls, i, mo;

        hist_join(joined, sizeof(joined));
        if (build_request(reqbody, sizeof(reqbody), g_model, g_maxtok,
                          g_use_tools, joined) < 0) {
            fprintf(stderr, "[claude] request body too large\n");
            return 0;
        }

        if (transport_send(reqbody, resp, sizeof(resp)) < 0)
            return 0;

        status = rtc_http_status(resp);
        body = rtc_http_body(resp);
        if (!body) {
            fprintf(stderr, "[claude] no HTTP body in response\n");
            return 0;
        }
        if (header_has_chunked(resp, body))
            dechunk_in_place((char *)body);

        if (status >= 400) {
            char emsg[512];
            if (get_json_string(body, "message", emsg, sizeof(emsg)))
                fprintf(stderr, "[claude] API error %d: %s\n", status, emsg);
            else
                fprintf(stderr, "[claude] API error %d\n", status);
            return 0;
        }

        if (!get_raw_span(body, "content", &cs, &clen)) {
            fprintf(stderr, "[claude] no content array in response\n");
            return 0;
        }
        if (clen > (int)sizeof(content_arr) - 1)
            clen = (int)sizeof(content_arr) - 1;
        memcpy(content_arr, cs, clen);
        content_arr[clen] = '\0';

        {
            char stop[32];
            get_json_string(body, "stop_reason", stop, sizeof(stop));

            ncalls = 0;
            collect_text_and_tools(content_arr, textout, sizeof(textout),
                                   calls, &ncalls, MAX_TOOLS);
            if (textout[0])
                printf("%s\n", textout);

            if (strcmp(stop, "tool_use") != 0 || ncalls == 0)
                return 1;   /* end_turn (or nothing to do) */
        }

        /* echo the assistant turn (with its tool_use blocks) verbatim */
        sprintf(asst, "{\"role\":\"assistant\",\"content\":%s}", content_arr);
        hist_add(asst);

        /* run each tool, collect tool_result blocks into one user turn */
        mo = sprintf(toolmsg, "{\"role\":\"user\",\"content\":[");
        for (i = 0; i < ncalls; i++) {
            static char toolout[TOOL_OUT_MAX];
            static char block[TRBLOCK_BUF];
            int is_err = 0, bl;

            execute_tool(&calls[i], toolout, sizeof(toolout), &is_err);
            bl = build_tool_result(block, sizeof(block), calls[i].id,
                                   toolout, is_err);
            if (bl < 0)
                continue;
            if (i > 0 && mo < (int)sizeof(toolmsg) - 2)
                toolmsg[mo++] = ',';
            if (mo + bl < (int)sizeof(toolmsg) - 4) {
                memcpy(toolmsg + mo, block, bl);
                mo += bl;
            }
        }
        toolmsg[mo++] = ']';
        toolmsg[mo++] = '}';
        toolmsg[mo] = '\0';
        hist_add(toolmsg);
    }

    fprintf(stderr, "[claude] tool-use loop hit the %d-iteration cap\n",
            MAX_ITERS);
    return 1;
}

/* ---- REPL ---- */

static void repl(void)
{
    static char line[4096];
    static char umsg[MSGOBJ_BUF];

    printf("Claude on the Amiga. Type /quit to exit.\n");
    for (;;) {
        printf("\nyou> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;
        {
            int l = (int)strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                line[--l] = '\0';
        }
        if (line[0] == '\0')
            continue;
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/q") == 0)
            break;
        if (build_user_text_message(umsg, sizeof(umsg), line) < 0) {
            fprintf(stderr, "[claude] line too long\n");
            continue;
        }
        hist_add(umsg);
        printf("claude> ");
        run_conversation();
    }
}

/* ---- argument parsing + main ---- */

static void usage(void)
{
    printf("claude - Anthropic Messages API client for AmigaOS\n\n");
    printf("Usage:\n");
    printf("  claude \"your prompt\"        one-shot\n");
    printf("  claude -i                     interactive REPL\n\n");
    printf("Options:\n");
    printf("  --proxy host:port   use the plain-HTTP host proxy (no AmiSSL)\n");
    printf("  --model NAME        model id (default %s)\n", DEF_MODEL);
    printf("  --max-tokens N      response token cap (default %ld)\n", DEF_MAXTOK);
    printf("  --yes               auto-approve write/run tools (scripted use)\n");
    printf("  --insecure          skip TLS certificate verification\n");
    printf("  --no-tools          disable the tool-use loop\n");
    printf("  -v, --verbose       print transport progress\n");
    printf("  -h, --help          this help\n\n");
    printf("Key (AmiSSL direct mode): ENV:ANTHROPIC_API_KEY or SYS:.claude/config\n");
}

int main(int argc, char **argv)
{
    static char umsg[MSGOBJ_BUF];
    const char *prompt = NULL;
    int interactive = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            char hp[160];
            char *colon;
            strncpy(hp, argv[++i], sizeof(hp) - 1);
            hp[sizeof(hp) - 1] = '\0';
            colon = strchr(hp, ':');
            if (colon) {
                *colon = '\0';
                g_proxy_port = atoi(colon + 1);
            }
            strncpy(g_proxy_host, hp, sizeof(g_proxy_host) - 1);
            g_proxy_host[sizeof(g_proxy_host) - 1] = '\0';
            g_use_proxy = 1;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            g_model = argv[++i];
        } else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc) {
            g_maxtok = atol(argv[++i]);
        } else if (strcmp(argv[i], "--yes") == 0) {
            g_yes = 1;
        } else if (strcmp(argv[i], "--insecure") == 0) {
            g_insecure = 1;
        } else if (strcmp(argv[i], "--no-tools") == 0) {
            g_use_tools = 0;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] != '-') {
            prompt = argv[i];
        } else {
            fprintf(stderr, "[claude] unknown option %s\n", argv[i]);
            usage();
            return 20;
        }
    }

    if (!g_use_proxy)
        load_api_key();

    if (interactive) {
        repl();
        amissl_cleanup();
        rtc_net_close();
        return 0;
    }

    if (!prompt) {
        usage();
        return 5;
    }

    if (build_user_text_message(umsg, sizeof(umsg), prompt) < 0) {
        fprintf(stderr, "[claude] prompt too long\n");
        return 20;
    }
    hist_add(umsg);
    {
        int ok = run_conversation();
        amissl_cleanup();
        rtc_net_close();
        return ok ? 0 : 20;
    }
}

#else /* ================= HOST_TEST: pure-function self-test ============= */

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

/* canned Anthropic-shaped responses (no key, no network needed) */
static const char *FIX_TEXT =
"{\"id\":\"msg_01\",\"type\":\"message\",\"role\":\"assistant\","
"\"model\":\"claude-opus-4-8\",\"content\":[{\"type\":\"text\","
"\"text\":\"Hello from the Amiga!\\nLine two.\"}],"
"\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":10}}";

static const char *FIX_TOOL =
"{\"id\":\"msg_02\",\"type\":\"message\",\"role\":\"assistant\","
"\"content\":[{\"type\":\"text\",\"text\":\"Let me write that file.\"},"
"{\"type\":\"tool_use\",\"id\":\"toolu_abc123\",\"name\":\"write_file\","
"\"input\":{\"path\":\"SYS:claude_tool_out.txt\",\"text\":\"hi there\"}}],"
"\"stop_reason\":\"tool_use\"}";

int main(void)
{
    static char textout[TEXTBUF];
    static char content_arr[CONTENTBUF];
    struct tool_call calls[MAX_TOOLS];
    const char *cs;
    int clen, ncalls;

    printf("claude.c host self-test\n");
    printf("=======================\n");

    /* 1. JSON escape/unescape round trip */
    {
        char esc[256], back[256];
        const char *orig = "say \"hi\"\n\tand \\slash";
        int el = json_escape(esc, sizeof(esc), orig);
        check("json_escape returns length", el > 0);
        check("escape has \\\" ", strstr(esc, "\\\"") != NULL);
        check("escape has \\n", strstr(esc, "\\n") != NULL);
        json_unescape(esc, (int)strlen(esc), back, sizeof(back));
        check("unescape round-trips", strcmp(back, orig) == 0);
    }

    /* 2. build a user message + request, look for the required fields */
    {
        static char umsg[MSGOBJ_BUF];
        static char req[REQBODY];
        build_user_text_message(umsg, sizeof(umsg), "Say hello");
        check("user msg has role user",
              strstr(umsg, "\"role\":\"user\"") != NULL);
        check("user msg has content",
              strstr(umsg, "\"content\":\"Say hello\"") != NULL);
        build_request(req, sizeof(req), DEF_MODEL, DEF_MAXTOK, 1, umsg);
        check("request has model claude-opus-4-8",
              strstr(req, "\"model\":\"claude-opus-4-8\"") != NULL);
        check("request has max_tokens", strstr(req, "\"max_tokens\":4096") != NULL);
        check("request has tools array", strstr(req, "\"tools\":[") != NULL);
        check("request has read_file tool", strstr(req, "read_file") != NULL);
        check("request has messages array", strstr(req, "\"messages\":[") != NULL);
    }

    /* 3. parse the text response fixture */
    {
        char stop[32];
        check("content span found in text fixture",
              get_raw_span(FIX_TEXT, "content", &cs, &clen));
        memcpy(content_arr, cs, clen);
        content_arr[clen] = '\0';
        get_json_string(FIX_TEXT, "stop_reason", stop, sizeof(stop));
        check("stop_reason is end_turn", strcmp(stop, "end_turn") == 0);
        ncalls = -1;
        collect_text_and_tools(content_arr, textout, sizeof(textout),
                               calls, &ncalls, MAX_TOOLS);
        check("no tool calls in text fixture", ncalls == 0);
        check("text extracted and unescaped",
              strcmp(textout, "Hello from the Amiga!\nLine two.") == 0);
    }

    /* 4. parse the tool_use response fixture */
    {
        char stop[32];
        get_raw_span(FIX_TOOL, "content", &cs, &clen);
        memcpy(content_arr, cs, clen);
        content_arr[clen] = '\0';
        get_json_string(FIX_TOOL, "stop_reason", stop, sizeof(stop));
        check("tool fixture stop_reason is tool_use",
              strcmp(stop, "tool_use") == 0);
        ncalls = 0;
        collect_text_and_tools(content_arr, textout, sizeof(textout),
                               calls, &ncalls, MAX_TOOLS);
        check("one tool call parsed", ncalls == 1);
        check("tool name write_file", strcmp(calls[0].name, "write_file") == 0);
        check("tool id captured", strcmp(calls[0].id, "toolu_abc123") == 0);
        check("preamble text still extracted",
              strcmp(textout, "Let me write that file.") == 0);
        {
            char path[128], text[128];
            get_json_string(calls[0].input, "path", path, sizeof(path));
            get_json_string(calls[0].input, "text", text, sizeof(text));
            check("tool input path parsed",
                  strcmp(path, "SYS:claude_tool_out.txt") == 0);
            check("tool input text parsed", strcmp(text, "hi there") == 0);
        }
    }

    /* 5. tool_result round-trip formatting */
    {
        char block[512];
        int bl = build_tool_result(block, sizeof(block), "toolu_abc123",
                                   "wrote 8 bytes\nok", 0);
        check("tool_result built", bl > 0);
        check("tool_result has type",
              strstr(block, "\"type\":\"tool_result\"") != NULL);
        check("tool_result has tool_use_id",
              strstr(block, "\"tool_use_id\":\"toolu_abc123\"") != NULL);
        check("tool_result escaped newline in content",
              strstr(block, "wrote 8 bytes\\nok") != NULL);
        /* an error result carries is_error */
        bl = build_tool_result(block, sizeof(block), "toolu_x", "denied", 1);
        check("error tool_result has is_error",
              strstr(block, "\"is_error\":true") != NULL);
    }

    /* 6. a wrapped user tool_result turn is well formed enough to send back */
    {
        char block[512], turn[700];
        build_tool_result(block, sizeof(block), "toolu_abc123", "file written", 0);
        sprintf(turn, "{\"role\":\"user\",\"content\":[%s]}", block);
        check("tool_result turn has role user",
              strstr(turn, "\"role\":\"user\"") != NULL);
        check("tool_result turn embeds the block id",
              strstr(turn, "toolu_abc123") != NULL);
    }

    printf("=======================\n");
    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}

#endif /* HOST_TEST */
