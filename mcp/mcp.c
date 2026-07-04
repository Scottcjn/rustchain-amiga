/*
 * mcp.c - A native MCP (Model Context Protocol) server for classic AmigaOS
 *         (m68k). RustChain Amiga Edition.
 *
 * This is the INVERSE of claude/client/claude.c. Where claude.c is an MCP-style
 * client that lets Claude drive the Amiga over the Anthropic API, this is a
 * stdio JSON-RPC 2.0 MCP *server*: a modern LLM agent (running on a desktop MCP
 * client) spawns this binary over a serial/pipe bridge, lists its tools, and
 * calls them to act on the Amiga's own filesystem and shell.
 *
 * Transport (the standard MCP stdio transport):
 *   - Newline-delimited JSON-RPC 2.0 messages on stdin/stdout.
 *   - Read one line (one JSON-RPC request), dispatch it, write exactly one
 *     JSON-RPC response line, then fflush(stdout). Notifications get no reply.
 *   - stdout is PURE protocol. Every log/debug line goes to stderr only.
 *
 * Methods implemented:
 *   initialize                -> protocolVersion, capabilities{tools:{}}, serverInfo
 *   notifications/initialized -> no-op (notification, no response)
 *   ping                      -> {} (liveness)
 *   tools/list                -> the tool catalogue with JSON Schemas
 *   tools/call                -> dispatch read_file / write_file / run_command,
 *                                return MCP content blocks [{"type":"text",...}]
 *
 * The JSON escape/parse helpers and the three tool handlers are lifted almost
 * verbatim from claude/client/claude.c (its collect/tool-use side), so the two
 * programs speak the same tool semantics from opposite ends of the wire.
 *
 * m68k rules (carried from claude.c / miner / tools):
 *   - C89-friendly: declarations first, no // comments, no C99 loop decls.
 *   - No 64-bit shifts, ILP32, big endian. Big buffers are static (BSS), never
 *     on the stack, and no libnix 'unsigned long __stack' global is declared.
 *   - Built at -m68020 (bebbo miscompiles __mulsi3/atoi at -m68000 -> HANGS).
 *
 * Build:
 *   Amiga hunk exe : see Makefile target `mcp` (docker cross toolchain)
 *   Host self-test : -DHOST_TEST (see Makefile target `host-test`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sizes (static buffers; stdio only, no sockets/TLS) ---- */
#define PROTO_VERSION   "2024-11-05"
#define SERVER_NAME     "amiga-mcp"
#define SERVER_VERSION  "1.0.0"

#define LINEBUF     65536   /* one inbound JSON-RPC request line */
#define RESPBUF     66000   /* one outbound JSON-RPC response line */
#define PARAMBUF    49152   /* the params object of a request */
#define ARGBUF      40960   /* the arguments object of a tools/call */
#define TEXTBUF     32768   /* a write_file text payload */
#define TOOL_OUT_MAX 16000  /* max tool output bytes we return */
#define ESCBUF      (TOOL_OUT_MAX * 2 + 64)
#define RESULTBUF   (ESCBUF + 512)
#define IDBUF       256     /* the raw JSON-RPC id token */

/* ================================================================== */
/* Pure JSON helpers (compiled for host self-test AND Amiga)          */
/* Copied from claude/client/claude.c so both ends share the same code */
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

/* Find "key" used as an object key (followed by a colon) and return a pointer
   just past that colon. Skips occurrences where the text is a string value
   rather than a key. Returns NULL if the key is not present. */
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

/* Find "key": followed by a balanced [..] or {..} and hand back the raw span
   (start pointer + length), string-aware. Returns 1 on success. */
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

/* Capture the raw JSON token that is the value of "key": a number, a literal
   (true/false/null), or a quoted string WITH its quotes preserved. This is how
   the JSON-RPC id is echoed back verbatim (it may be a number or a string).
   Returns 1 on success. */
static int get_raw_token(const char *json, const char *key,
                         char *out, int outlen)
{
    const char *p = find_key(json, key);
    int o = 0;

    if (outlen < 1) return 0;
    out[0] = '\0';
    if (!p) return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"') {
        out[o++] = *p++;
        while (*p && o < outlen - 2) {
            if (*p == '\\' && p[1]) { out[o++] = *p++; out[o++] = *p++; }
            else if (*p == '"') { out[o++] = *p++; break; }
            else out[o++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
               o < outlen - 1)
            out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* the tool catalogue offered to the agent (MCP inputSchema JSON Schema) */
static const char *TOOLS_JSON =
"[{\"name\":\"read_file\",\"description\":\"Read a text file from the Amiga "
"filesystem. Give an AmigaDOS path like SYS:S/startup-sequence.\","
"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
"\"string\",\"description\":\"AmigaDOS path to read\"}},\"required\":"
"[\"path\"]}},"
"{\"name\":\"write_file\",\"description\":\"Write text to a file on the Amiga "
"filesystem, overwriting it. Give path and text.\",\"inputSchema\":{\"type\":"
"\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":"
"\"AmigaDOS path to write\"},\"text\":{\"type\":\"string\",\"description\":"
"\"file contents\"}},\"required\":[\"path\",\"text\"]}},"
"{\"name\":\"run_command\",\"description\":\"Run an AmigaDOS shell command and "
"return its captured output. Give cmd.\",\"inputSchema\":{\"type\":\"object\","
"\"properties\":{\"cmd\":{\"type\":\"string\",\"description\":\"AmigaDOS "
"command line\"}},\"required\":[\"cmd\"]}}]";

/* ================================================================== */
/* Tool implementations. Two variants: real Amiga dos.library, and a  */
/* host stdio/popen variant for the self-test (which runs real cmds). */
/* ================================================================== */

#ifndef HOST_TEST

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* read a text file via dos.library Open/Read */
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

/* write a text file via dos.library Open/Write (overwrites) */
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

/* run an AmigaDOS command via Execute(), capturing its output to a T: file
   which is then read back. Mirrors claude.c's tool_run_command. */
static int tool_run_command(const char *cmd, char *out, int outlen)
{
    BPTR outfh, rfh;
    long rc, n = 0;

    outfh = Open((STRPTR)"T:mcp_cmd.out", MODE_NEWFILE);
    if (!outfh) {
        strcpy(out, "ERROR: cannot open T:mcp_cmd.out");
        return 0;
    }
    rc = Execute((STRPTR)cmd, 0, outfh);
    Close(outfh);

    rfh = Open((STRPTR)"T:mcp_cmd.out", MODE_OLDFILE);
    if (rfh) {
        n = Read(rfh, out, outlen - 1);
        Close(rfh);
        if (n < 0)
            n = 0;
    }
    out[n] = '\0';
    DeleteFile((STRPTR)"T:mcp_cmd.out");
    if (rc == 0 && n == 0)
        sprintf(out, "(command '%s' produced no output; it may not have run)",
                cmd);
    return 1;
}

#else /* HOST_TEST: host implementations (stdio + popen) */

static int tool_read_file(const char *path, char *out, int outlen)
{
    FILE *f;
    size_t n;

    f = fopen(path, "rb");
    if (!f) {
        sprintf(out, "ERROR: cannot open %s for reading", path);
        return 0;
    }
    n = fread(out, 1, (size_t)(outlen - 1), f);
    fclose(f);
    out[n] = '\0';
    return 1;
}

static int tool_write_file(const char *path, const char *text,
                           char *out, int outlen)
{
    FILE *f;
    int len = (int)strlen(text);
    size_t w;

    (void)outlen;
    f = fopen(path, "wb");
    if (!f) {
        sprintf(out, "ERROR: cannot open %s for writing", path);
        return 0;
    }
    w = fwrite(text, 1, (size_t)len, f);
    fclose(f);
    if ((int)w != len) {
        sprintf(out, "ERROR: wrote %d of %d bytes to %s", (int)w, len, path);
        return 0;
    }
    sprintf(out, "OK: wrote %d bytes to %s", len, path);
    return 1;
}

static int tool_run_command(const char *cmd, char *out, int outlen)
{
    FILE *p;
    size_t n;

    p = popen(cmd, "r");
    if (!p) {
        sprintf(out, "ERROR: cannot run '%s'", cmd);
        return 0;
    }
    n = fread(out, 1, (size_t)(outlen - 1), p);
    pclose(p);
    out[n] = '\0';
    if (n == 0)
        sprintf(out, "(command '%s' produced no output)", cmd);
    return 1;
}

#endif /* HOST_TEST */

/* ================================================================== */
/* JSON-RPC response builders (shared)                                */
/* ================================================================== */

/* Wrap a result object in the JSON-RPC 2.0 envelope, echoing id verbatim. */
static int build_response(char *dst, int dstlen, const char *id_raw,
                          const char *result_json)
{
    int n = snprintf(dst, dstlen,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}",
        (id_raw && id_raw[0]) ? id_raw : "null", result_json);
    if (n < 0 || n >= dstlen) return -1;
    return n;
}

/* Build a JSON-RPC 2.0 error envelope. */
static int build_error(char *dst, int dstlen, const char *id_raw,
                       int code, const char *message)
{
    static char esc[256];
    int n;

    json_escape(esc, sizeof(esc), message);
    n = snprintf(dst, dstlen,
        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,"
        "\"message\":\"%s\"}}",
        (id_raw && id_raw[0]) ? id_raw : "null", code, esc);
    if (n < 0 || n >= dstlen) return -1;
    return n;
}

/* Build a tools/call result: MCP content blocks. Returns length or -1. */
static int build_tool_result(char *dst, int dstlen, const char *text,
                             int is_error)
{
    static char esc[ESCBUF];
    int n;

    if (json_escape(esc, sizeof(esc), text) < 0) {
        /* the tool output is capped below ESCBUF/2 so this cannot happen, but
           degrade safely rather than emit malformed JSON */
        strcpy(esc, "(output too large to encode)");
    }
    n = snprintf(dst, dstlen,
        "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":%s}",
        esc, is_error ? "true" : "false");
    if (n < 0 || n >= dstlen) return -1;
    return n;
}

/* Dispatch a tools/call by name against the params object. Fills result_json
   (a full MCP tools/call result object). Returns 1 always (errors are reported
   as isError:true content, per MCP tool-error convention). */
static void dispatch_tool_call(const char *params, char *result_json,
                               int result_len)
{
    static char args[ARGBUF];
    static char toolout[TOOL_OUT_MAX];
    char name[64];
    const char *as;
    int alen, is_err = 0;

    name[0] = '\0';
    get_json_string(params, "name", name, sizeof(name));

    args[0] = '\0';
    if (get_raw_span(params, "arguments", &as, &alen)) {
        if (alen > (int)sizeof(args) - 1)
            alen = (int)sizeof(args) - 1;
        memcpy(args, as, alen);
        args[alen] = '\0';
    }

    if (strcmp(name, "read_file") == 0) {
        char path[512];
        if (!get_json_string(args, "path", path, sizeof(path))) {
            strcpy(toolout, "ERROR: missing required argument 'path'");
            is_err = 1;
        } else if (!tool_read_file(path, toolout, sizeof(toolout))) {
            is_err = 1;
        }
    } else if (strcmp(name, "write_file") == 0) {
        char path[512];
        static char text[TEXTBUF];
        if (!get_json_string(args, "path", path, sizeof(path))) {
            strcpy(toolout, "ERROR: missing required argument 'path'");
            is_err = 1;
        } else {
            text[0] = '\0';
            get_json_string(args, "text", text, sizeof(text));
            if (!tool_write_file(path, text, toolout, sizeof(toolout)))
                is_err = 1;
        }
    } else if (strcmp(name, "run_command") == 0) {
        char cmd[1024];
        if (!get_json_string(args, "cmd", cmd, sizeof(cmd))) {
            strcpy(toolout, "ERROR: missing required argument 'cmd'");
            is_err = 1;
        } else if (!tool_run_command(cmd, toolout, sizeof(toolout))) {
            is_err = 1;
        }
    } else {
        sprintf(toolout, "ERROR: unknown tool '%s'", name);
        is_err = 1;
    }

    build_tool_result(result_json, result_len, toolout, is_err);
}

/* ================================================================== */
/* Top-level request dispatch (shared by Amiga server + host self-test)*/
/* Returns 1 if 'resp' holds a response line to send, 0 for a          */
/* notification (send nothing).                                        */
/* ================================================================== */

static int handle_request(const char *req, char *resp, int resplen)
{
    static char params[PARAMBUF];
    static char result[RESULTBUF];
    char method[64];
    char id[IDBUF];
    const char *ps;
    int plen, have_id;

    method[0] = '\0';
    get_json_string(req, "method", method, sizeof(method));
    have_id = get_raw_token(req, "id", id, sizeof(id));
    if (!have_id)
        id[0] = '\0';

    /* extract the params object (if any) */
    params[0] = '\0';
    if (get_raw_span(req, "params", &ps, &plen)) {
        if (plen > (int)sizeof(params) - 1)
            plen = (int)sizeof(params) - 1;
        memcpy(params, ps, plen);
        params[plen] = '\0';
    }

    /* notifications (no id, method starts with "notifications/") get no reply */
    if (strncmp(method, "notifications/", 14) == 0)
        return 0;

    if (strcmp(method, "initialize") == 0) {
        char pv[32];
        pv[0] = '\0';
        get_json_string(params, "protocolVersion", pv, sizeof(pv));
        if (pv[0] == '\0')
            strcpy(pv, PROTO_VERSION);
        snprintf(result, sizeof(result),
            "{\"protocolVersion\":\"%s\",\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"}}",
            pv, SERVER_NAME, SERVER_VERSION);
        build_response(resp, resplen, id, result);
        return 1;
    }

    if (strcmp(method, "ping") == 0) {
        build_response(resp, resplen, id, "{}");
        return 1;
    }

    if (strcmp(method, "tools/list") == 0) {
        snprintf(result, sizeof(result), "{\"tools\":%s}", TOOLS_JSON);
        build_response(resp, resplen, id, result);
        return 1;
    }

    if (strcmp(method, "tools/call") == 0) {
        dispatch_tool_call(params, result, sizeof(result));
        build_response(resp, resplen, id, result);
        return 1;
    }

    /* if there was no id at all and the method was unknown, it was a stray
       notification; say nothing. Otherwise report method-not-found. */
    if (!have_id)
        return 0;
    build_error(resp, resplen, id, -32601, "Method not found");
    return 1;
}

/* ================================================================== */
/* Line reader + server loop (shared: stdio only, fully portable, so   */
/* the host build can drive a real wire session with `host_test --serve`)*/
/* ================================================================== */

/* Read one newline-delimited line from stdin into buf (without the newline).
   Returns length >= 0, or -1 at EOF with no data. Overlong lines are read to
   the end of the physical line but truncated to the buffer. */
static int read_line(char *buf, int buflen)
{
    int o = 0, c, saw = 0;

    for (;;) {
        c = getchar();
        if (c == EOF)
            break;
        saw = 1;
        if (c == '\n')
            break;
        if (c == '\r')
            continue;
        if (o < buflen - 1)
            buf[o++] = (char)c;
    }
    buf[o] = '\0';
    if (!saw && o == 0)
        return -1;
    return o;
}

static void run_server(void)
{
    static char line[LINEBUF];
    static char resp[RESPBUF];
    int n;

    for (;;) {
        n = read_line(line, sizeof(line));
        if (n < 0)
            break;              /* EOF: client closed the pipe */
        if (n == 0)
            continue;           /* blank line between messages */
        if (handle_request(line, resp, sizeof(resp))) {
            fputs(resp, stdout);
            fputc('\n', stdout);
            fflush(stdout);     /* stdout is the wire: flush every response */
        }
    }
}

#ifndef HOST_TEST

int main(void)
{
    /* stdout is the JSON-RPC wire; nothing but responses may go there. A short
       banner on stderr is fine and helps when watching the serial bridge. */
    fprintf(stderr, "[mcp] Amiga MCP server ready (stdio JSON-RPC 2.0)\n");
    run_server();
    return 0;
}

#else /* ================= HOST_TEST: protocol self-test ================= */

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(int argc, char **argv)
{
    static char resp[RESPBUF];
    int r;

    /* `host_test --serve` runs the real stdio wire loop (host tool impls), so
       an actual MCP client / a piped session can be exercised end to end. */
    if (argc > 1 && strcmp(argv[1], "--serve") == 0) {
        run_server();
        return 0;
    }

    printf("mcp.c host self-test\n");
    printf("====================\n");

    /* 1. initialize handshake */
    {
        const char *req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"test\",\"version\":\"0\"}}}";
        r = handle_request(req, resp, sizeof(resp));
        check("initialize produced a response", r == 1);
        check("initialize echoes jsonrpc 2.0",
              strstr(resp, "\"jsonrpc\":\"2.0\"") != NULL);
        check("initialize echoes id 1", strstr(resp, "\"id\":1") != NULL);
        check("initialize returns protocolVersion",
              strstr(resp, "\"protocolVersion\":\"2024-11-05\"") != NULL);
        check("initialize advertises tools capability",
              strstr(resp, "\"tools\":{}") != NULL);
        check("initialize returns serverInfo name",
              strstr(resp, "\"name\":\"amiga-mcp\"") != NULL);
        check("initialize result has no error",
              strstr(resp, "\"error\"") == NULL);
    }

    /* 2. notifications/initialized is a no-op (no response) */
    {
        const char *req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
        r = handle_request(req, resp, sizeof(resp));
        check("notifications/initialized yields no response", r == 0);
    }

    /* 3. tools/list returns all three tools with inputSchema */
    {
        const char *req =
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
        r = handle_request(req, resp, sizeof(resp));
        check("tools/list produced a response", r == 1);
        check("tools/list echoes id 2", strstr(resp, "\"id\":2") != NULL);
        check("tools/list has tools array",
              strstr(resp, "\"result\":{\"tools\":[") != NULL);
        check("tools/list lists read_file",
              strstr(resp, "\"name\":\"read_file\"") != NULL);
        check("tools/list lists write_file",
              strstr(resp, "\"name\":\"write_file\"") != NULL);
        check("tools/list lists run_command",
              strstr(resp, "\"name\":\"run_command\"") != NULL);
        check("tools/list uses inputSchema",
              strstr(resp, "\"inputSchema\"") != NULL);
    }

    /* 4. tools/call write_file then read_file against /tmp (real host FS) */
    {
        const char *wreq =
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"write_file\",\"arguments\":{\"path\":"
        "\"/tmp/mcp_amiga_test.txt\",\"text\":\"hello from mcp\\nline two\"}}}";
        r = handle_request(wreq, resp, sizeof(resp));
        check("write_file produced a response", r == 1);
        check("write_file content is text block",
              strstr(resp, "\"content\":[{\"type\":\"text\"") != NULL);
        check("write_file reports OK",
              strstr(resp, "OK: wrote") != NULL);
        check("write_file isError false",
              strstr(resp, "\"isError\":false") != NULL);
    }
    {
        const char *rreq =
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"read_file\",\"arguments\":{\"path\":"
        "\"/tmp/mcp_amiga_test.txt\"}}}";
        r = handle_request(rreq, resp, sizeof(resp));
        check("read_file produced a response", r == 1);
        check("read_file returns the written text (escaped newline)",
              strstr(resp, "hello from mcp\\nline two") != NULL);
        check("read_file isError false",
              strstr(resp, "\"isError\":false") != NULL);
    }

    /* 5. tools/call run_command actually runs a shell command on the host */
    {
        const char *creq =
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"run_command\",\"arguments\":{\"cmd\":"
        "\"echo mcp_ran_ok\"}}}";
        r = handle_request(creq, resp, sizeof(resp));
        check("run_command produced a response", r == 1);
        check("run_command captured stdout",
              strstr(resp, "mcp_ran_ok") != NULL);
        check("run_command isError false",
              strstr(resp, "\"isError\":false") != NULL);
    }

    /* 6. tools/call with a missing argument -> isError true, but valid JSON-RPC */
    {
        const char *breq =
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"read_file\",\"arguments\":{}}}";
        r = handle_request(breq, resp, sizeof(resp));
        check("missing-arg call still responds", r == 1);
        check("missing-arg call flags isError true",
              strstr(resp, "\"isError\":true") != NULL);
        check("missing-arg call names the missing arg",
              strstr(resp, "missing required argument 'path'") != NULL);
    }

    /* 7. unknown tool name -> isError true content */
    {
        const char *ureq =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"no_such_tool\",\"arguments\":{}}}";
        r = handle_request(ureq, resp, sizeof(resp));
        check("unknown tool responds", r == 1);
        check("unknown tool flags isError true",
              strstr(resp, "\"isError\":true") != NULL);
        check("unknown tool names it",
              strstr(resp, "unknown tool 'no_such_tool'") != NULL);
    }

    /* 8. unknown method with an id -> JSON-RPC error -32601 */
    {
        const char *ereq =
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"bogus/method\"}";
        r = handle_request(ereq, resp, sizeof(resp));
        check("unknown method responds", r == 1);
        check("unknown method returns error object",
              strstr(resp, "\"error\":{") != NULL);
        check("unknown method code -32601",
              strstr(resp, "\"code\":-32601") != NULL);
    }

    /* 9. ping -> empty result */
    {
        const char *preq =
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}";
        r = handle_request(preq, resp, sizeof(resp));
        check("ping responds", r == 1);
        check("ping returns empty result",
              strstr(resp, "\"result\":{}") != NULL);
    }

    /* 10. string id is echoed verbatim (JSON-RPC ids may be strings) */
    {
        const char *sreq =
        "{\"jsonrpc\":\"2.0\",\"id\":\"abc-42\",\"method\":\"ping\"}";
        r = handle_request(sreq, resp, sizeof(resp));
        check("string-id ping responds", r == 1);
        check("string id echoed with quotes",
              strstr(resp, "\"id\":\"abc-42\"") != NULL);
    }

    printf("====================\n");
    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}

#endif /* HOST_TEST */
