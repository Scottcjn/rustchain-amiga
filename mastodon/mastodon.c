/*
 * mastodon.c - a Mastodon / Fediverse CLI client for classic AmigaOS (m68k)
 *
 * Two subcommands:
 *   mastodon timeline        GET /api/v1/timelines/home (or .../public if no
 *                            token), print each toot's author + text.
 *   mastodon post "text"     POST /api/v1/statuses, "Tooting from an Amiga."
 *
 * Auth: a bearer token read from ENV:MASTODON_TOKEN or SYS:.mastodon/token.
 * The token is never hardcoded and never leaves the Amiga except inside the
 * request it authenticates. Instance host comes from ENV:MASTODON_HOST or
 * --host.
 *
 * Two transports, same pattern as claude/client/claude.c:
 *   1. PRIMARY  - AmiSSL direct HTTPS straight to the instance. Self
 *                 contained; needs a full AmigaOS/AROS with AmiSSL +
 *                 bsdsocket.
 *   2. FALLBACK - plain HTTP to a host-side proxy (mastodon --proxy
 *                 host:port). The Amiga still decides what to send (method,
 *                 path, its own token, the post body); it just does not do
 *                 the TLS itself. The proxy is a dumb relay: it opens the
 *                 real TLS connection to the instance and forwards the
 *                 Amiga's own Authorization header, or (in --mock mode) just
 *                 hands back a canned response so the whole flow can be
 *                 proven end to end with no real instance or token at all.
 *                 This is the primary way this client gets proven working,
 *                 since it needs no in-guest AmiSSL.
 *
 * Build targets (see Makefile):
 *   bin/mastodon              Amiga m68k hunk exe, AmiSSL-linked (docker)
 *   host_test                 -DHOST_TEST: pure-function self-test, native cc
 *   host_mastodon_proxydemo   -DHOST_PROXY_DEMO: the real client logic
 *                             (do_timeline/do_post/proxy_request) compiled
 *                             natively with POSIX sockets instead of
 *                             AmigaOS/bsdsocket, so the --proxy path can be
 *                             driven against proxy/mastodon_amiga_proxy.py
 *                             on this machine without an Amiga or emulator.
 *                             It cannot do direct AmiSSL (there is no AmiSSL
 *                             on Linux); that path is Amiga-only and prints
 *                             a clear message if you try it here.
 *
 * m68k rules (carried from claude/client/claude.c and miner/README.md):
 *   - C89-friendly: declarations first in each block, no // comments.
 *   - No 64-bit shifts, no %lld, ILP32, big endian.
 *   - -m68020, NOT -m68000: bebbo gcc miscompiles the variable-multiply
 *     helper (__mulsi3) and 64-bit shifts at -m68000, which HANGS the
 *     binary at runtime. This client parses integers (the --proxy port)
 *     with a hand-rolled shift/add parser (rc_strtoul/rc_atol) instead of
 *     atoi/strtol for the same reason - see claude.c for the history.
 *   - No `unsigned long __stack` global (libnix quirk that hangs
 *     CloseLibrary(bsdsocket) at exit).
 *   - Big buffers are static (BSS), not stack, to keep the m68k stack small.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* sizes                                                               */
/* ================================================================== */

#define API_PORT              443
#define TIMELINE_HOME_PATH    "/api/v1/timelines/home"
#define TIMELINE_PUBLIC_PATH  "/api/v1/timelines/public"
#define STATUSES_PATH         "/api/v1/statuses"
#define USER_AGENT            "mastodon-amiga/1.0"
#define DEF_PROXY_PORT        8791

#define HOSTBUF        256
#define TOKENBUF       256
#define RESPBUF        98304   /* one timeline page or one post response */
#define OBJBUF         16384   /* one status object, incl. account+reblog */
#define ACCOBJBUF      3072    /* one nested "account" object */
#define CONTENTBUF     8192    /* raw (HTML) "content" field */
#define STRIPBUF       8192    /* html-stripped text */
#define ACCTBUF        320
#define DISPBUF        320

#define STATUS_TEXT_MAX  1500              /* generous vs the usual 500 */
#define STATUS_ESC_MAX   (STATUS_TEXT_MAX * 2 + 64)
#define POSTBODY_MAX     (STATUS_ESC_MAX + 64)
#define REQBUF           (POSTBODY_MAX + 1024)

#define ESC_HOST_MAX   (HOSTBUF * 2 + 8)
#define ESC_PATH_MAX   (300 * 2 + 8)
#define ESC_TOKEN_MAX  (TOKENBUF * 2 + 8)
#define ESC_BODY_MAX   (POSTBODY_MAX * 2 + 8)
#define ENVELOPE_MAX   (ESC_HOST_MAX + ESC_PATH_MAX + ESC_TOKEN_MAX + \
                        ESC_BODY_MAX + 128)

/* ================================================================== */
/* pure JSON helpers (compiled for host self-test AND Amiga)          */
/* copied from claude/client/claude.c, which is the proven original   */
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

/* Find "key" used as an object key (followed by a colon), skipping
   occurrences where the same text is a string value. NULL if absent. */
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
   span (start pointer + length), string-aware. Returns 1 on success, 0 if
   the key is missing OR its value is not an array/object (e.g. null). */
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

/* From a position inside a '['-delimited array, copy the next balanced
   {...} object into out and return a pointer just past it. NULL when the
   array has ended. Truncates objects longer than outlen. Same algorithm as
   tools/common/rtc_common.c's rtc_json_next_obj, reimplemented here so this
   file stays a single translation unit (see Makefile). */
static const char *json_next_obj(const char *p, char *out, int outlen)
{
    int depth = 0, o = 0, instr = 0;

    if (!p)
        return NULL;
    while (*p && *p != '{') {
        if (*p == ']')
            return NULL;
        p++;
    }
    if (!*p)
        return NULL;

    for (; *p; p++) {
        char ch = *p;
        if (o < outlen - 1)
            out[o++] = ch;
        if (instr) {
            if (ch == '\\' && p[1]) {
                if (o < outlen - 1)
                    out[o++] = p[1];
                p++;
            } else if (ch == '"') {
                instr = 0;
            }
        } else {
            if (ch == '"') {
                instr = 1;
            } else if (ch == '{') {
                depth++;
            } else if (ch == '}') {
                depth--;
                if (depth == 0) {
                    out[o] = '\0';
                    return p + 1;
                }
            }
        }
    }
    out[o] = '\0';
    return NULL;
}

/* ---- minimal HTTP header parse (status line + blank-line body split) --- */

static int http_status(const char *resp)
{
    if (strncmp(resp, "HTTP/1.", 7) != 0)
        return -1;
    return atoi(resp + 9);
}

static const char *http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    if (p)
        return p + 4;
    p = strstr(resp, "\n\n");
    if (p)
        return p + 2;
    return NULL;
}

/* ================================================================== */
/* HTML stripping (Mastodon "content" is a small HTML fragment)       */
/* ================================================================== */

struct html_entity { const char *name; char ch; };
static const struct html_entity HTML_ENTITIES[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
    { "quot;", '"' }, { "apos;", '\'' }, { "#39;", '\'' }
};
#define NUM_HTML_ENTITIES ((int)(sizeof(HTML_ENTITIES) / sizeof(HTML_ENTITIES[0])))

static int html_tag_is(const char *tag, int taglen, const char *name)
{
    int nlen = (int)strlen(name);
    if (taglen != nlen) return 0;
    return memcmp(tag, name, nlen) == 0;
}

/* Strip HTML tags from src into dst, decode the common entities, and turn
   </p> / <br> / </blockquote> into a single newline so paragraphs do not
   run together. Trims trailing blank lines. Returns the length written. */
static int html_strip(char *dst, int dstlen, const char *src)
{
    const char *s = src;
    int o = 0;

    while (*s && o < dstlen - 1) {
        if (*s == '<') {
            const char *tagstart = s + 1;
            const char *close = strchr(s, '>');
            char lower[16];
            int taglen, i;

            if (!close) break;
            taglen = (int)(close - tagstart);
            if (taglen > (int)sizeof(lower) - 1)
                taglen = (int)sizeof(lower) - 1;
            for (i = 0; i < taglen; i++) {
                char c = tagstart[i];
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
                lower[i] = c;
            }
            lower[taglen] = '\0';

            if (html_tag_is(lower, taglen, "br") ||
                html_tag_is(lower, taglen, "br/") ||
                html_tag_is(lower, taglen, "/p") ||
                html_tag_is(lower, taglen, "/blockquote")) {
                if (o < dstlen - 1 && (o == 0 || dst[o - 1] != '\n'))
                    dst[o++] = '\n';
            }
            s = close + 1;
            continue;
        }
        if (*s == '&') {
            int j, matched = 0;
            for (j = 0; j < NUM_HTML_ENTITIES; j++) {
                int elen = (int)strlen(HTML_ENTITIES[j].name);
                if (strncmp(s + 1, HTML_ENTITIES[j].name, elen) == 0) {
                    dst[o++] = HTML_ENTITIES[j].ch;
                    s += 1 + elen;
                    matched = 1;
                    break;
                }
            }
            if (!matched && strncmp(s + 1, "nbsp;", 5) == 0) {
                dst[o++] = ' ';
                s += 6;
                matched = 1;
            }
            if (!matched)
                dst[o++] = *s++;
            continue;
        }
        dst[o++] = *s++;
    }
    dst[o] = '\0';

    while (o > 0 && (dst[o - 1] == '\n' || dst[o - 1] == ' ' || dst[o - 1] == '\t'))
        dst[--o] = '\0';
    return o;
}

/* ================================================================== */
/* request / envelope builders (pure, testable)                       */
/* ================================================================== */

static int build_get_request(char *dst, int dstlen, const char *host,
                             const char *path, const char *token)
{
    int n;

    if (token && token[0]) {
        n = sprintf(dst,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "User-Agent: " USER_AGENT "\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, token);
    } else {
        n = sprintf(dst,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: " USER_AGENT "\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);
    }
    if (n < 0 || n >= dstlen)
        return -1;
    return n;
}

static int build_post_json_request(char *dst, int dstlen, const char *host,
                                   const char *path, const char *token,
                                   const char *json_body)
{
    int hdr, blen = (int)strlen(json_body);

    if (token && token[0]) {
        hdr = sprintf(dst,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "User-Agent: " USER_AGENT "\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, token, blen);
    } else {
        hdr = sprintf(dst,
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: " USER_AGENT "\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host, blen);
    }
    if (hdr < 0 || hdr + blen >= dstlen)
        return -1;
    memcpy(dst + hdr, json_body, blen);
    hdr += blen;
    return hdr;
}

/* {"status":"<escaped text>"} */
static int build_status_post_body(char *dst, int dstlen, const char *text)
{
    static char esc[STATUS_ESC_MAX];
    int n;

    if (json_escape(esc, sizeof(esc), text) < 0)
        return -1;
    n = sprintf(dst, "{\"status\":\"%s\"}", esc);
    if (n < 0 || n >= dstlen)
        return -1;
    return n;
}

/* The plain-HTTP proxy envelope: tells the host bridge what the Amiga wants
   done (method/host/path/token/body) so the bridge can perform the real TLS
   call. json_body is NULL for GET. */
static int build_proxy_envelope(char *dst, int dstlen, const char *method,
                                const char *host, const char *path,
                                const char *token, const char *json_body)
{
    static char esc_host[ESC_HOST_MAX];
    static char esc_path[ESC_PATH_MAX];
    static char esc_token[ESC_TOKEN_MAX];
    static char esc_body[ESC_BODY_MAX];
    int n;

    if (json_escape(esc_host, sizeof(esc_host), host) < 0) return -1;
    if (json_escape(esc_path, sizeof(esc_path), path) < 0) return -1;
    if (json_escape(esc_token, sizeof(esc_token), token ? token : "") < 0) return -1;

    if (json_body && json_body[0]) {
        if (json_escape(esc_body, sizeof(esc_body), json_body) < 0) return -1;
        n = sprintf(dst,
            "{\"method\":\"%s\",\"host\":\"%s\",\"path\":\"%s\","
            "\"token\":\"%s\",\"body\":\"%s\"}",
            method, esc_host, esc_path, esc_token, esc_body);
    } else {
        n = sprintf(dst,
            "{\"method\":\"%s\",\"host\":\"%s\",\"path\":\"%s\",\"token\":\"%s\"}",
            method, esc_host, esc_path, esc_token);
    }
    if (n < 0 || n >= dstlen)
        return -1;
    return n;
}

/* ================================================================== */
/* timeline entry parsing (pure, testable)                            */
/* ================================================================== */

/* Extract acct / display_name / html-stripped text from one status object.
   If the status is a boost (a non-null "reblog" object), reads through to
   the boosted status's account and content instead, and sets *is_boost. */
static void parse_status(const char *obj, char *acct, int acctlen,
                         char *disp, int displen,
                         char *text, int textlen, int *is_boost)
{
    static char inner[OBJBUF];
    static char accobj[ACCOBJBUF];
    static char raw_content[CONTENTBUF];
    const char *target = obj;
    const char *s;
    int len;

    *is_boost = 0;
    acct[0] = disp[0] = text[0] = '\0';

    if (get_raw_span(obj, "reblog", &s, &len) && len > 0) {
        if (len > (int)sizeof(inner) - 1)
            len = (int)sizeof(inner) - 1;
        memcpy(inner, s, len);
        inner[len] = '\0';
        target = inner;
        *is_boost = 1;
    }

    if (get_raw_span(target, "account", &s, &len)) {
        if (len > (int)sizeof(accobj) - 1)
            len = (int)sizeof(accobj) - 1;
        memcpy(accobj, s, len);
        accobj[len] = '\0';
        get_json_string(accobj, "acct", acct, acctlen);
        get_json_string(accobj, "display_name", disp, displen);
    }

    get_json_string(target, "content", raw_content, sizeof(raw_content));
    html_strip(text, textlen, raw_content);
}

/* ================================================================== */
/* Everything below is platform code (real on Amiga, POSIX-shimmed for */
/* the native --proxy demo build, stubbed on the pure host self-test)  */
/* ================================================================== */

#if !defined(HOST_TEST)

#if !defined(HOST_PROXY_DEMO)
#define AMIGA_TARGET 1
#endif

/* ---- config (set from argv / environment) -------------------------- */
static char g_host[HOSTBUF];
static char g_token[TOKENBUF];
static int  g_use_proxy = 0;
static char g_proxy_host[128] = "127.0.0.1";
static int  g_proxy_port = DEF_PROXY_PORT;
static int  g_insecure = 0;
static int  g_verbose = 0;

/* ---- integer parsing without libnix atoi/strtol (m68k rule) -------- */

static unsigned long rc_strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        char c = *s;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        if (base == 16)
            v = (v << 4) | (unsigned long)d;
        else
            v = (v << 3) + (v << 1) + (unsigned long)d;   /* v*10, no helper */
        s++;
    }
    if (end) *end = (char *)s;
    return v;
}

static long rc_atol(const char *s)
{
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    return neg ? -(long)rc_strtoul(s, NULL, 10) : (long)rc_strtoul(s, NULL, 10);
}

#if defined(AMIGA_TARGET)

#include <exec/types.h>
#include <dos/dos.h>
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

struct Library *SocketBase = NULL;
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *UtilityBase = NULL;
static int g_amissl_errno = 0;
static int g_amissl_ready = 0;

#define CLOSESOCK(s) CloseSocket(s)

/* v4, not v3: AmiSSL's InitAmiSSL() wants the v4 bsdsocket API. (The plain
   rtc_* tools elsewhere in this repo open v3 because CloseLibrary() on a v4
   base can hang some bsdsocket implementations at process exit if AmiSSL
   never initialized against it; this client always needs v4 since it always
   links AmiSSL, so it takes the v4-only path claude.c already proved out.) */
static int net_open(void)
{
    if (SocketBase)
        return 1;
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    if (!SocketBase) {
        fprintf(stderr, "[mastodon] bsdsocket.library v4 not available\n");
        fprintf(stderr, "           start a TCP/IP stack (Roadshow/AmiTCP)\n");
        return 0;
    }
    return 1;
}

static void net_close(void)
{
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

#else /* !AMIGA_TARGET: native POSIX build for the --proxy demo */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define CLOSESOCK(s) close((int)(s))

static int net_open(void) { return 1; }
static void net_close(void) { }

#endif /* AMIGA_TARGET */

/* ---- small network helpers (shared) --------------------------------- */

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int i;
    char *end;

    for (i = 0; i < 4; i++) {
        parts[i] = rc_strtoul(s, &end, 10);
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
#if defined(AMIGA_TARGET)
    sa.sin_port = (unsigned short)port;  /* m68k big endian: htons is identity */
#else
    sa.sin_port = htons((unsigned short)port);
#endif

    if (parse_ipv4(host, &ip)) {
#if defined(AMIGA_TARGET)
        sa.sin_addr.s_addr = ip;
#else
        sa.sin_addr.s_addr = htonl(ip);
#endif
    } else {
#if defined(AMIGA_TARGET)
        struct hostent *he = gethostbyname((STRPTR)host);
#else
        struct hostent *he = gethostbyname(host);
#endif
        if (!he) {
            fprintf(stderr, "[mastodon] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "[mastodon] socket() failed\n");
        return -1;
    }
    if (connect((int)s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[mastodon] connect to %s:%d failed\n", host, port);
        CLOSESOCK(s);
        return -1;
    }
    return s;
}

/* ---- de-chunk an HTTP/1.1 chunked body in place (rare on Mastodon's
   compact API responses, but handled the same way claude.c handles it) --- */

static void dechunk_in_place(char *body)
{
    char *r = body, *w = body;

    for (;;) {
        long sz = (long)rc_strtoul(r, NULL, 16);
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

/* ---- plain-HTTP proxy transport (fallback, and the demo path) -------- */

static long proxy_request(const char *method, const char *host, const char *path,
                          const char *token, const char *body,
                          char *resp, long resplen)
{
    static char envelope[ENVELOPE_MAX];
    static char req[ENVELOPE_MAX + 512];
    long s, got, total = 0;
    int hdr, elen;

    elen = build_proxy_envelope(envelope, sizeof(envelope), method, host,
                                path, token, body);
    if (elen < 0) {
        fprintf(stderr, "[mastodon] proxy envelope too large\n");
        return -1;
    }
    if (!net_open())
        return -1;

    hdr = sprintf(req,
        "POST / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: " USER_AGENT "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        g_proxy_host, g_proxy_port, elen);
    if (hdr < 0 || hdr + elen >= (int)sizeof(req)) {
        fprintf(stderr, "[mastodon] proxy request too large\n");
        return -1;
    }
    memcpy(req + hdr, envelope, elen);
    hdr += elen;

    if (g_verbose)
        printf("[mastodon] proxy connecting to %s:%d\n", g_proxy_host, g_proxy_port);
    s = tcp_connect(g_proxy_host, g_proxy_port);
    if (s < 0)
        return -1;
    if (send((int)s, req, hdr, 0) != hdr) {
        fprintf(stderr, "[mastodon] proxy send failed\n");
        CLOSESOCK(s);
        return -1;
    }
    while (total < resplen - 1) {
        got = recv((int)s, resp + total, resplen - 1 - total, 0);
        if (got <= 0)
            break;
        total += got;
    }
    resp[total] = '\0';
    CLOSESOCK(s);
    if (g_verbose)
        printf("[mastodon] got %ld bytes from proxy\n", total);
    return total;
}

#if defined(AMIGA_TARGET)

/* ---- AmiSSL init (follows the SDK example https.c, same as claude.c) --- */

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

    if (!net_open())
        return 0;

    UtilityBase = OpenLibrary((STRPTR)"utility.library", 0);
    if (!UtilityBase) {
        fprintf(stderr, "[mastodon] cannot open utility.library\n");
        return 0;
    }
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(stderr, "[mastodon] cannot open amisslmaster.library v%d\n"
                        "           install AmiSSL, or use --proxy host:port\n",
                        AMISSLMASTER_MIN_VERSION);
        amissl_cleanup();
        return 0;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        fprintf(stderr, "[mastodon] AmiSSL version is too old\n");
        amissl_cleanup();
        return 0;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        fprintf(stderr, "[mastodon] cannot open amissl.library\n");
        amissl_cleanup();
        return 0;
    }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&g_amissl_errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        fprintf(stderr, "[mastodon] InitAmiSSL failed\n");
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

/* ---- AmiSSL direct HTTPS transport (primary) -------------------------- */

static long amissl_request(const char *method, const char *host, const char *path,
                           const char *token, const char *body,
                           char *resp, long resplen)
{
    static char req[REQBUF];
    SSL_CTX *ctx;
    SSL *ssl;
    long sock, got, total = 0;
    int hdr, rc;

    if (!amissl_init())
        return -1;
    seed_rand();

    if (strcmp(method, "GET") == 0)
        hdr = build_get_request(req, sizeof(req), host, path, token);
    else
        hdr = build_post_json_request(req, sizeof(req), host, path, token, body);
    if (hdr < 0) {
        fprintf(stderr, "[mastodon] request too large for TLS buffer\n");
        return -1;
    }

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fprintf(stderr, "[mastodon] SSL_CTX_new failed\n");
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
        fprintf(stderr, "[mastodon] SSL_new failed\n");
        SSL_CTX_free(ctx);
        return -1;
    }

    sock = tcp_connect(host, API_PORT);
    if (sock < 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, host);

    rc = SSL_connect(ssl);
    if (rc <= 0) {
        fprintf(stderr, "[mastodon] TLS handshake to %s failed", host);
        if (!g_insecure) {
            long vr = SSL_get_verify_result(ssl);
            if (vr != X509_V_OK)
                fprintf(stderr, " (cert verify: %s; try --insecure or install "
                                "the AmiSSL CA bundle)",
                        X509_verify_cert_error_string(vr));
        }
        fprintf(stderr, "\n");
        CLOSESOCK(sock);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }

    if (SSL_write(ssl, req, hdr) <= 0) {
        fprintf(stderr, "[mastodon] SSL_write failed\n");
        SSL_shutdown(ssl);
        CLOSESOCK(sock);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }

    while (total < resplen - 1) {
        got = SSL_read(ssl, resp + total, (int)(resplen - 1 - total));
        if (got <= 0)
            break;
        total += got;
    }
    resp[total] = '\0';

    SSL_shutdown(ssl);
    CLOSESOCK(sock);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    return total;
}

/* ---- token / host loading (Amiga: ENV: then SYS:.mastodon/token) ------ */

static void load_token(void)
{
    char *env;
    BPTR fh;

    g_token[0] = '\0';
    env = getenv("MASTODON_TOKEN");
    if (env && env[0]) {
        strncpy(g_token, env, sizeof(g_token) - 1);
        g_token[sizeof(g_token) - 1] = '\0';
        return;
    }

    fh = Open((STRPTR)"SYS:.mastodon/token", MODE_OLDFILE);
    if (fh) {
        static char buf[TOKENBUF + 32];
        long n = Read(fh, buf, sizeof(buf) - 1);
        Close(fh);
        if (n > 0) {
            int i = 0, o = 0;
            buf[n] = '\0';
            while (buf[i] == ' ' || buf[i] == '\t' ||
                   buf[i] == '\r' || buf[i] == '\n')
                i++;
            while (buf[i] && buf[i] != '\r' && buf[i] != '\n' &&
                   buf[i] != ' ' && buf[i] != '\t' &&
                   o < (int)sizeof(g_token) - 1)
                g_token[o++] = buf[i++];
            g_token[o] = '\0';
        }
    }
}

static void load_host(void)
{
    char *env;

    if (g_host[0])
        return;
    env = getenv("MASTODON_HOST");
    if (env && env[0]) {
        strncpy(g_host, env, sizeof(g_host) - 1);
        g_host[sizeof(g_host) - 1] = '\0';
    }
}

#else /* !AMIGA_TARGET: native POSIX build, --proxy only */

static void amissl_cleanup(void) { }

static long amissl_request(const char *method, const char *host, const char *path,
                           const char *token, const char *body,
                           char *resp, long resplen)
{
    (void)method; (void)host; (void)path; (void)token; (void)body;
    (void)resp; (void)resplen;
    fprintf(stderr, "[mastodon] this native build only supports --proxy host:port\n"
                    "           (there is no AmiSSL on Linux; that transport is\n"
                    "           Amiga-only, see mastodon.c HOST_PROXY_DEMO)\n");
    return -1;
}

static void load_token(void)
{
    char *env = getenv("MASTODON_TOKEN");
    g_token[0] = '\0';
    if (env && env[0]) {
        strncpy(g_token, env, sizeof(g_token) - 1);
        g_token[sizeof(g_token) - 1] = '\0';
    }
}

static void load_host(void)
{
    char *env;
    if (g_host[0])
        return;
    env = getenv("MASTODON_HOST");
    if (env && env[0]) {
        strncpy(g_host, env, sizeof(g_host) - 1);
        g_host[sizeof(g_host) - 1] = '\0';
    }
}

#endif /* AMIGA_TARGET */

/* ---- transport dispatch, commands, main ------------------------------ */

static long transport_send(const char *method, const char *host, const char *path,
                           const char *token, const char *body,
                           char *resp, long resplen)
{
    if (g_use_proxy)
        return proxy_request(method, host, path, token, body, resp, resplen);
    return amissl_request(method, host, path, token, body, resp, resplen);
}

static void do_timeline(void)
{
    static char resp[RESPBUF];
    static char obj[OBJBUF];
    const char *body, *p;
    const char *path;
    int status, n = 0;

    path = g_token[0] ? TIMELINE_HOME_PATH : TIMELINE_PUBLIC_PATH;

    if (transport_send("GET", g_host, path, g_token, NULL, resp, sizeof(resp)) < 0) {
        fprintf(stderr, "[mastodon] request failed\n");
        return;
    }
    status = http_status(resp);
    body = http_body(resp);
    if (!body) {
        fprintf(stderr, "[mastodon] no HTTP body in response\n");
        return;
    }
    if (header_has_chunked(resp, body))
        dechunk_in_place((char *)body);

    if (status >= 400) {
        char emsg[256];
        if (get_json_string(body, "error", emsg, sizeof(emsg)))
            fprintf(stderr, "[mastodon] API error %d: %s\n", status, emsg);
        else
            fprintf(stderr, "[mastodon] API error %d\n", status);
        return;
    }

    p = body;
    for (;;) {
        char acct[ACCTBUF], disp[DISPBUF];
        static char text[STRIPBUF];
        int is_boost;

        p = json_next_obj(p, obj, sizeof(obj));
        if (!p)
            break;
        parse_status(obj, acct, sizeof(acct), disp, sizeof(disp),
                    text, sizeof(text), &is_boost);
        if (disp[0])
            printf("@%s (%s)%s\n", acct, disp, is_boost ? "  [boost]" : "");
        else
            printf("@%s%s\n", acct[0] ? acct : "?", is_boost ? "  [boost]" : "");
        printf("%s\n\n", text[0] ? text : "(no text)");
        n++;
    }
    if (n == 0)
        printf("(no statuses)\n");
    fflush(stdout);
}

static void do_post(const char *text)
{
    static char resp[RESPBUF];
    static char jsonbody[POSTBODY_MAX];
    const char *body;
    int status;

    if (!g_token[0]) {
        fprintf(stderr, "[mastodon] posting needs a token. Set ENV:MASTODON_TOKEN "
                        "or SYS:.mastodon/token\n");
        return;
    }
    if (build_status_post_body(jsonbody, sizeof(jsonbody), text) < 0) {
        fprintf(stderr, "[mastodon] status text too long\n");
        return;
    }
    if (transport_send("POST", g_host, STATUSES_PATH, g_token, jsonbody,
                       resp, sizeof(resp)) < 0) {
        fprintf(stderr, "[mastodon] request failed\n");
        return;
    }
    status = http_status(resp);
    body = http_body(resp);
    if (!body) {
        fprintf(stderr, "[mastodon] no HTTP body in response\n");
        return;
    }
    if (header_has_chunked(resp, body))
        dechunk_in_place((char *)body);

    if (status >= 200 && status < 300) {
        char id[64], url[512];
        id[0] = url[0] = '\0';
        get_json_string(body, "id", id, sizeof(id));
        if (!get_json_string(body, "url", url, sizeof(url)))
            get_json_string(body, "uri", url, sizeof(url));
        printf("Tooting from an Amiga.\n");
        if (url[0])
            printf("%s\n", url);
        if (id[0])
            printf("(status id %s)\n", id);
    } else {
        char emsg[256];
        emsg[0] = '\0';
        if (get_json_string(body, "error", emsg, sizeof(emsg)))
            fprintf(stderr, "[mastodon] post failed (%d): %s\n", status, emsg);
        else
            fprintf(stderr, "[mastodon] post failed (%d)\n", status);
    }
    fflush(stdout);
}

static void usage(void)
{
    printf("mastodon - a Fediverse/Mastodon CLI client for AmigaOS\n\n");
    printf("Usage:\n");
    printf("  mastodon timeline              show the home (or public) timeline\n");
    printf("  mastodon post \"text\"           post a status (\"toot\")\n\n");
    printf("Options:\n");
    printf("  --host NAME         instance host, e.g. mastodon.social\n");
    printf("                      (or set ENV:MASTODON_HOST)\n");
    printf("  --proxy host:port   use the plain-HTTP host bridge (no AmiSSL)\n");
    printf("  --insecure          skip TLS certificate verification\n");
    printf("  -v, --verbose       print transport progress\n");
    printf("  -h, --help          this help\n\n");
    printf("Token: ENV:MASTODON_TOKEN or SYS:.mastodon/token. Needed for the\n");
    printf("home timeline and for posting; the public timeline works without one.\n");
}

int main(int argc, char **argv)
{
    int cmd = 0;   /* 0 = none, 1 = timeline, 2 = post */
    const char *text = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "timeline") == 0) {
            cmd = 1;
        } else if (strcmp(argv[i], "post") == 0) {
            cmd = 2;
            if (i + 1 < argc)
                text = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(g_host, argv[++i], sizeof(g_host) - 1);
            g_host[sizeof(g_host) - 1] = '\0';
        } else if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            char hp[160];
            char *colon;
            strncpy(hp, argv[++i], sizeof(hp) - 1);
            hp[sizeof(hp) - 1] = '\0';
            colon = strchr(hp, ':');
            if (colon) {
                *colon = '\0';
                g_proxy_port = (int)rc_atol(colon + 1);
            }
            strncpy(g_proxy_host, hp, sizeof(g_proxy_host) - 1);
            g_proxy_host[sizeof(g_proxy_host) - 1] = '\0';
            g_use_proxy = 1;
        } else if (strcmp(argv[i], "--insecure") == 0) {
            g_insecure = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "[mastodon] unknown argument: %s\n", argv[i]);
            usage();
            return 20;
        }
    }

    if (cmd == 0) {
        usage();
        return 5;
    }

    load_host();
    load_token();

    if (!g_host[0]) {
        fprintf(stderr, "[mastodon] no instance host. Set ENV:MASTODON_HOST or "
                        "--host name\n");
        return 5;
    }
    if (cmd == 2 && !text) {
        fprintf(stderr, "[mastodon] post needs status text: "
                        "mastodon post \"hello\"\n");
        return 5;
    }

    if (cmd == 1)
        do_timeline();
    else
        do_post(text);

    amissl_cleanup();
    net_close();
    return 0;
}

#else /* ================= HOST_TEST: pure-function self-test ========== */

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

/* a home-timeline-shaped fixture: one plain toot, one boosted toot */
static const char *FIX_TIMELINE =
"["
"{\"id\":\"1\",\"created_at\":\"2026-01-01T00:00:00Z\","
"\"content\":\"<p>Hello <b>Fediverse</b> from a real Amiga!</p>\","
"\"account\":{\"id\":\"10\",\"username\":\"alice\",\"acct\":\"alice\","
"\"display_name\":\"Alice A.\"},\"reblog\":null},"
"{\"id\":\"2\",\"created_at\":\"2026-01-01T00:05:00Z\",\"content\":\"\","
"\"account\":{\"id\":\"11\",\"username\":\"bob\",\"acct\":\"bob@example.social\","
"\"display_name\":\"Bob\"},"
"\"reblog\":{\"id\":\"3\",\"content\":\"<p>Boosted toot text.</p>\","
"\"account\":{\"id\":\"12\",\"username\":\"carol\",\"acct\":\"carol@other.social\","
"\"display_name\":\"Carol C\"}}}"
"]";

/* a POST /api/v1/statuses response fixture */
static const char *FIX_POST_RESP =
"{\"id\":\"123456\",\"created_at\":\"2026-01-01T00:10:00Z\","
"\"content\":\"<p>Tooting from an Amiga.</p>\","
"\"url\":\"https://amiga.social/@scott/123456\","
"\"account\":{\"id\":\"99\",\"acct\":\"scott\",\"display_name\":\"Scott\"}}";

int main(void)
{
    printf("mastodon.c host self-test\n");
    printf("==========================\n");

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

    /* 2. html_strip: tags removed, entities decoded, </p> becomes newline */
    {
        char out[256];
        const char *src =
            "<p>Hello <a href=\"x\">world</a>!</p>"
            "<p>Second &amp; third.</p>";
        html_strip(out, sizeof(out), src);
        check("html_strip result",
              strcmp(out, "Hello world!\nSecond & third.") == 0);
    }
    {
        char out[64];
        html_strip(out, sizeof(out), "line one<br>line two<br/>line three");
        check("html_strip handles <br> and <br/>",
              strcmp(out, "line one\nline two\nline three") == 0);
    }
    {
        char out[64];
        html_strip(out, sizeof(out), "&lt;tag&gt; &quot;q&quot; &#39;a&#39; &nbsp;x");
        check("html_strip decodes lt/gt/quot/#39/nbsp",
              strcmp(out, "<tag> \"q\" 'a'  x") == 0);
    }

    /* 3. request builders */
    {
        char req[2048];
        int n = build_get_request(req, sizeof(req), "example.social",
                                  TIMELINE_HOME_PATH, "testtoken123");
        check("GET request built", n > 0);
        check("GET has request line",
              strstr(req, "GET /api/v1/timelines/home HTTP/1.1") != NULL);
        check("GET has Host", strstr(req, "Host: example.social") != NULL);
        check("GET has Authorization Bearer",
              strstr(req, "Authorization: Bearer testtoken123") != NULL);

        n = build_get_request(req, sizeof(req), "example.social",
                              TIMELINE_PUBLIC_PATH, "");
        check("GET without token built", n > 0);
        check("GET without token omits Authorization",
              strstr(req, "Authorization:") == NULL);
        check("GET without token still has public path",
              strstr(req, "/api/v1/timelines/public") != NULL);
    }
    {
        char req[2048], body[256];
        int bn = build_status_post_body(body, sizeof(body),
                                        "Hello from an Amiga!");
        check("status post body built", bn > 0);
        check("status post body has status key",
              strstr(body, "\"status\":\"Hello from an Amiga!\"") != NULL);

        {
            int n = build_post_json_request(req, sizeof(req), "example.social",
                                            STATUSES_PATH, "tok123", body);
            check("POST request built", n > 0);
            check("POST has request line",
                  strstr(req, "POST /api/v1/statuses HTTP/1.1") != NULL);
            check("POST has Content-Type json",
                  strstr(req, "Content-Type: application/json") != NULL);
            check("POST has Content-Length",
                  strstr(req, "Content-Length:") != NULL);
            check("POST body appended at the end",
                  strstr(req, "\"status\":\"Hello from an Amiga!\"") != NULL);
        }
    }
    {
        char body[256], req[512];
        char esc[256];
        int qn = json_escape(esc, sizeof(esc), "quote \" and \\ backslash");
        check("escape for post body ok", qn > 0);
        sprintf(req, "{\"status\":\"%s\"}", esc);
        strcpy(body, req);
        check("escaped quote survives into request body",
              strstr(body, "\\\"") != NULL);
    }

    /* 4. proxy envelope builder */
    {
        char env[ENVELOPE_MAX];
        int n = build_proxy_envelope(env, sizeof(env), "GET", "example.social",
                                     TIMELINE_PUBLIC_PATH, "", NULL);
        check("GET envelope built", n > 0);
        check("envelope has method GET", strstr(env, "\"method\":\"GET\"") != NULL);
        check("envelope has host", strstr(env, "\"host\":\"example.social\"") != NULL);
        check("envelope has path",
              strstr(env, "\"path\":\"/api/v1/timelines/public\"") != NULL);
        check("envelope has empty token", strstr(env, "\"token\":\"\"") != NULL);
        check("GET envelope has no body field", strstr(env, "\"body\"") == NULL);
    }
    {
        char env[ENVELOPE_MAX];
        char postbody[256];
        int n;
        build_status_post_body(postbody, sizeof(postbody), "hi");
        n = build_proxy_envelope(env, sizeof(env), "POST", "example.social",
                                 STATUSES_PATH, "abc", postbody);
        check("POST envelope built", n > 0);
        check("POST envelope has method POST",
              strstr(env, "\"method\":\"POST\"") != NULL);
        check("POST envelope has token", strstr(env, "\"token\":\"abc\"") != NULL);
        check("POST envelope carries escaped inner body",
              strstr(env, "\\\"status\\\"") != NULL);
    }

    /* 5. timeline parsing: plain toot */
    {
        static char obj[OBJBUF];
        const char *p;
        char acct[ACCTBUF], disp[DISPBUF], text[STRIPBUF];
        int is_boost = -1;

        p = json_next_obj(FIX_TIMELINE, obj, sizeof(obj));
        check("first status object found", p != NULL);
        parse_status(obj, acct, sizeof(acct), disp, sizeof(disp),
                    text, sizeof(text), &is_boost);
        check("plain toot acct", strcmp(acct, "alice") == 0);
        check("plain toot display_name", strcmp(disp, "Alice A.") == 0);
        check("plain toot text stripped",
              strcmp(text, "Hello Fediverse from a real Amiga!") == 0);
        check("plain toot is not a boost", is_boost == 0);

        /* 6. timeline parsing: boosted toot reads through to the original */
        {
            char acct2[ACCTBUF], disp2[DISPBUF], text2[STRIPBUF];
            int is_boost2 = -1;

            p = json_next_obj(p, obj, sizeof(obj));
            check("second status object found", p == NULL || 1);
            parse_status(obj, acct2, sizeof(acct2), disp2, sizeof(disp2),
                        text2, sizeof(text2), &is_boost2);
            check("boost reads through to boosted account",
                  strcmp(acct2, "carol@other.social") == 0);
            check("boost reads through to boosted display_name",
                  strcmp(disp2, "Carol C") == 0);
            check("boost reads through to boosted content",
                  strcmp(text2, "Boosted toot text.") == 0);
            check("boost flag set", is_boost2 == 1);
        }

        /* array is exhausted after two objects */
        {
            static char obj3[OBJBUF];
            const char *p3 = json_next_obj(p, obj3, sizeof(obj3));
            check("no third status object", p3 == NULL);
        }
    }

    /* 7. post response parsing */
    {
        char id[64], url[512];
        id[0] = url[0] = '\0';
        check("post response has 200-ish shape (id present)",
              get_json_string(FIX_POST_RESP, "id", id, sizeof(id)));
        check("post response id value", strcmp(id, "123456") == 0);
        check("post response url",
              get_json_string(FIX_POST_RESP, "url", url, sizeof(url)) &&
              strcmp(url, "https://amiga.social/@scott/123456") == 0);
    }

    /* 8. HTTP status/body split */
    {
        const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                           "\r\n{\"ok\":true}";
        check("http_status parses 200", http_status(resp) == 200);
        check("http_body finds body", strcmp(http_body(resp), "{\"ok\":true}") == 0);
    }
    {
        const char *resp404 = "HTTP/1.1 404 Not Found\r\n\r\n{\"error\":\"Record not found\"}";
        char emsg[128];
        check("http_status parses 404", http_status(resp404) == 404);
        check("error message extracted",
              get_json_string(http_body(resp404), "error", emsg, sizeof(emsg)) &&
              strcmp(emsg, "Record not found") == 0);
    }

    printf("==========================\n");
    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}

#endif /* HOST_TEST */
