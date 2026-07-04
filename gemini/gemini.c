/*
 * gemini.c - a Gemini protocol client/browser for classic AmigaOS (m68k)
 * and AROS.
 *
 * Protocol facts (Gemini spec, gemini://gemini.circumlunar.space/docs/specification.gmi,
 * do not deviate):
 *   TLS on port 1965 (default). Request: the full absolute URL + "\r\n",
 *   max 1024 bytes. Response: a single header line "<2-digit><space><meta>
 *   \r\n", then (for 2x only) the body, with no length framing -- the
 *   server just closes the connection when it is done sending.
 *   Status categories: 1x INPUT, 2x SUCCESS, 3x REDIRECT, 4x TEMPORARY
 *   FAILURE, 5x PERMANENT FAILURE, 6x CLIENT CERTIFICATE REQUIRED.
 *   text/gemini is a small line-oriented markup: "# "/"## "/"### " headings,
 *   "=> url [label]" links, "* " list items, "```" toggles a preformatted
 *   block (no line inside it is reinterpreted), ">" is a quote line.
 *
 * Two transports (same as claude/client/claude.c, same reasons):
 *   1. PRIMARY  - AmiSSL direct TLS to the target host. Self contained,
 *                 needs a full AmigaOS/AROS with AmiSSL + bsdsocket.
 *   2. FALLBACK - `gemini --proxy host:port` opens a plain TCP connection to
 *                 a small host-side bridge (proxy/gemini_amiga_proxy.py)
 *                 that does the TLS. Lets this be tested without in-guest
 *                 AmiSSL. The wire format to the proxy is: a "<host> <port>
 *                 \n" header line, then the exact same "<url>\r\n" request
 *                 line the direct path would have sent over TLS; the proxy
 *                 relays the raw response back byte for byte, so the
 *                 response-parsing code is identical for both transports.
 *
 * TLS trust: Gemini capsules are almost all self-signed (the spec expects
 * clients to do TOFU -- trust On first use -- certificate pinning, not CA
 * validation). This client does not implement a TOFU store; it defaults to
 * SSL_VERIFY_NONE. Pass --strict-tls to require a CA-validated cert, which
 * will fail against most real capsules -- that is expected, not a bug.
 *
 * m68k rules (carried from claude/client/claude.c and miner/README.md):
 *   - C89-friendly: declarations first, no // comments, no C99 loop decls.
 *   - No 64-bit shifts, no %lld, ILP32, big endian.
 *   - Large buffers are static/global (BSS), never big stack arrays: TLS
 *     and this program's own line/URL scratch space are stack-hungry, and
 *     the default AmigaOS CLI stack is small.
 *   - No libnix atoi/atol/strtol on numbers that vary at runtime (bebbo gcc
 *     6.5 miscompiles the __mulsi3 helper it emits for them at -m68000 and
 *     the process HANGS). This file uses its own shift/add rc_strtoul,
 *     copied from claude.c, and only ever builds at -m68020 anyway (see
 *     the Makefile), which has native MULU.L/MULS.L -- but the convention
 *     is kept for consistency with the rest of this repo.
 *   - Do not declare a libnix `unsigned long __stack` global (it makes
 *     CloseLibrary(bsdsocket) hang at exit).
 *   - fflush(stdout) after printing: libnix fully buffers redirected stdout.
 *
 * Vendored helpers: vendor/rtc_common.c (copied from claude/client/vendor/,
 * itself from tools/common/). This client only uses rtc_net_open()/
 * rtc_net_close() (bsdsocket.library v3 -- v4 hangs CloseLibrary() on AROS)
 * and rtc_check_break(). Its own URL parser, status-line parser, and
 * gemtext renderer are pure C89 below, compiled unconditionally so they can
 * be exercised by the host self-test with no network and no Amiga headers.
 *
 * Build:
 *   Amiga hunk exe : see Makefile target `bin/gemini` (docker cross + AmiSSL)
 *   Host self-test : -DHOST_TEST (see Makefile target `host-test`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/rtc_common.h"

/* -------------------------------------------------------------------- */
/* sizes                                                                 */
/* -------------------------------------------------------------------- */

#define GEM_DEFAULT_PORT   1965
#define GEM_HOSTBUF        256
#define GEM_PATHBUF        1024
#define GEM_URLBUF_MAX     1400   /* > GEM_HOSTBUF + GEM_PATHBUF + margin */
#define GEM_LINE_MAX       4096
#define GEM_LINKURL_MAX    600
#define GEM_MAX_LINKS      128
#define GEM_RESPBUF        131072   /* 128K: a generously sized capsule page */
#define GEM_OUTBUF         147456   /* rendered output; a bit bigger than body */
#define GEM_MAX_SEGS       48
#define GEM_SEG_MAX        256

typedef struct {
    char ref[GEM_LINKURL_MAX];   /* raw, unresolved reference text */
} gem_link_t;

/* ====================================================================== */
/* Pure logic (compiled for HOST_TEST *and* the Amiga build)              */
/* ====================================================================== */

/* bebbo/libnix atoi/atol/strtol hang on m68k targets built at -m68000 (see
   the file header). Shift/add, no variable*variable 32-bit multiply. Copied
   from claude/client/claude.c. */
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

static int gem_is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* ---- URL parsing: gemini://host[:port][/path][?query] ---- */

/* Accepts "gemini://..." (case-insensitive), rejects any other explicit
   scheme, and also accepts a bare "host[:port][/path]" with no scheme at
   all (a convenience for the command line). host/path are always NUL
   terminated within the given buffers; path always starts with '/' and
   defaults to "/". A '#fragment' suffix, if present, is stripped (Gemini
   requests do not carry fragments). Returns 1 ok, 0 on a bad/unsupported
   URL. */
int gem_parse_url(const char *url, char *host, int hostlen,
                  int *port, char *path, int pathlen)
{
    const char *p = url;
    int hl = 0;
    char *hash;

    if (!url || !host || !port || !path || hostlen < 2 || pathlen < 2)
        return 0;

    if ((p[0] == 'g' || p[0] == 'G') && (p[1] == 'e' || p[1] == 'E') &&
        (p[2] == 'm' || p[2] == 'M') && (p[3] == 'i' || p[3] == 'I') &&
        (p[4] == 'n' || p[4] == 'N') && (p[5] == 'i' || p[5] == 'I') &&
        p[6] == ':' && p[7] == '/' && p[8] == '/') {
        p += 9;
    } else if (strstr(p, "://") != NULL) {
        return 0;   /* some other scheme; not ours to fetch */
    }
    /* else: bare host[:port][/path], scheme omitted -- accept it */

    if (*p == '\0' || *p == '/')
        return 0;   /* no host */

    while (*p && *p != ':' && *p != '/' && *p != '?' && hl < hostlen - 1)
        host[hl++] = *p++;
    host[hl] = '\0';
    if (hl == 0)
        return 0;

    *port = GEM_DEFAULT_PORT;
    if (*p == ':') {
        char *pend;
        p++;
        *port = (int)rc_strtoul(p, &pend, 10);
        if (*port <= 0 || *port > 65535)
            return 0;
        p = pend;
    }

    if (*p == '\0') {
        path[0] = '/';
        path[1] = '\0';
    } else if (*p == '?') {
        int pl = 1;
        path[0] = '/';
        while (*p && pl < pathlen - 1)
            path[pl++] = *p++;
        path[pl] = '\0';
    } else {
        int pl = 0;
        while (*p && pl < pathlen - 1)
            path[pl++] = *p++;
        path[pl] = '\0';
    }

    hash = strchr(path, '#');
    if (hash) *hash = '\0';
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }
    return 1;
}

/* Build "gemini://host[:port]path" into out. Returns length, or -1 if it
   does not fit (should not happen: callers size out at GEM_URLBUF_MAX,
   which is provably larger than GEM_HOSTBUF + ":65535" + GEM_PATHBUF). */
int gem_build_url(const char *host, int port, const char *path,
                  char *out, int outlen)
{
    int n;
    if (port == GEM_DEFAULT_PORT)
        n = sprintf(out, "gemini://%s%s", host, path);
    else
        n = sprintf(out, "gemini://%s:%d%s", host, port, path);
    if (n < 0 || n >= outlen)
        return -1;
    return n;
}

/* ---- Gemini response header line: "<2-digit><space><meta>\r\n" ---- */

int gem_status_code(const char *resp)
{
    if (!resp || !gem_is_digit(resp[0]) || !gem_is_digit(resp[1]))
        return -1;
    return (resp[0] - '0') * 10 + (resp[1] - '0');
}

/* Copies the meta text (after the mandatory space, before the line end)
   into out. Accepts a bare LF as a lenient fallback to a strict CRLF.
   Returns 1 ok, 0 if the line is not a well-formed status line. */
int gem_status_meta(const char *resp, char *out, int outlen)
{
    const char *p, *end;
    int len;

    if (!resp || !out || outlen < 1)
        return 0;
    out[0] = '\0';
    if (!gem_is_digit(resp[0]) || !gem_is_digit(resp[1]))
        return 0;
    p = resp + 2;
    if (*p != ' ')
        return 0;   /* the space is mandatory */
    p++;
    end = strstr(p, "\r\n");
    if (!end) {
        end = strchr(p, '\n');
        if (!end)
            return 0;
    }
    len = (int)(end - p);
    if (len > outlen - 1)
        len = outlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* Pointer to the body, just past the header line's terminator. NULL if the
   header line never terminates (still arriving, or malformed). */
const char *gem_body(const char *resp)
{
    const char *p;
    if (!resp)
        return NULL;
    p = strstr(resp, "\r\n");
    if (p)
        return p + 2;
    p = strchr(resp, '\n');
    if (p)
        return p + 1;
    return NULL;
}

/* ---- percent encode/decode (for submitting INPUT responses as a query) --- */

int gem_url_encode(const char *in, char *out, int outlen)
{
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *s = (const unsigned char *)in;
    int o = 0;

    for (; *s; s++) {
        unsigned char c = *s;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                        c == '_' || c == '~';
        if (unreserved) {
            if (o >= outlen - 1) return -1;
            out[o++] = (char)c;
        } else {
            if (o >= outlen - 3) return -1;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    if (o >= outlen) return -1;
    out[o] = '\0';
    return o;
}

static int gem_hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int gem_url_decode(const char *in, char *out, int outlen)
{
    int o = 0;
    while (*in) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = gem_hex_val(in[1]);
            int lo = gem_hex_val(in[2]);
            if (hi >= 0 && lo >= 0) {
                if (o >= outlen - 1) return -1;
                out[o++] = (char)((hi << 4) | lo);
                in += 3;
                continue;
            }
        }
        if (o >= outlen - 1) return -1;
        out[o++] = *in++;
    }
    out[o] = '\0';
    return o;
}

/* ---- relative URL resolution (a small subset of RFC 3986 5.3, enough for
   gemtext links: "." and ".." dot-segments, scheme-relative "//host/path",
   absolute "/path", query-only "?q", and plain relative "sibling.gmi") --- */

/* Remove "." and ".." dot-segments from a '/'-rooted path. Keeps a trailing
   slash if the input had one. A ".." past the root is silently dropped
   (lenient, matches what real browsers do) rather than treated as an error. */
static void gem_normalize_path(const char *in, char *out, int outlen)
{
    static char segs[GEM_MAX_SEGS][GEM_SEG_MAX];
    int nseg = 0;
    const char *p = in;
    int trailing_slash;
    int inlen = (int)strlen(in);
    int o, i;

    trailing_slash = (inlen > 1 && in[inlen - 1] == '/');

    if (*p == '/') p++;

    while (*p) {
        static char seg[GEM_SEG_MAX];
        int sl = 0;
        while (*p && *p != '/' && sl < GEM_SEG_MAX - 1)
            seg[sl++] = *p++;
        seg[sl] = '\0';
        if (*p == '/') p++;

        if (sl == 0) {
            /* consecutive slash: nothing to push */
        } else if (strcmp(seg, ".") == 0) {
            /* current directory: drop it */
        } else if (strcmp(seg, "..") == 0) {
            if (nseg > 0) nseg--;
        } else if (nseg < GEM_MAX_SEGS) {
            strcpy(segs[nseg], seg);
            nseg++;
        }
    }

    o = 0;
    if (outlen > 0) out[o++] = '/';
    for (i = 0; i < nseg; i++) {
        int l = (int)strlen(segs[i]);
        if (i > 0 && o < outlen - 1) out[o++] = '/';
        if (o + l < outlen) { memcpy(out + o, segs[i], l); o += l; }
    }
    if (trailing_slash && nseg > 0 && o < outlen - 1 && out[o - 1] != '/')
        out[o++] = '/';
    if (o >= outlen) o = outlen - 1;
    out[o] = '\0';
}

/* RFC 3986 5.3 merge: base_path up to and including its last '/', then
   ref_path appended (not yet dot-segment normalized). */
static void gem_merge_path(const char *base_path, const char *ref_path,
                           char *out, int outlen)
{
    const char *slash = strrchr(base_path, '/');
    int dirlen = slash ? (int)(slash - base_path) + 1 : 0;
    int reflen = (int)strlen(ref_path);
    int o = 0;

    if (dirlen == 0) {
        if (outlen > 0) out[o++] = '/';
    } else {
        if (dirlen > outlen - 1) dirlen = outlen - 1;
        memcpy(out, base_path, dirlen);
        o = dirlen;
    }
    if (o + reflen < outlen) { memcpy(out + o, ref_path, reflen); o += reflen; }
    out[o] = '\0';
}

/* Resolve ref_in against (base_host, base_port, base_path). Handles:
     gemini://host/path   (or any other "scheme://" -> rejected, not ours)
     //host/path           (scheme-relative)
     /abs/path[?q]         (absolute path, same host)
     ?query                (same path, new query)
     relative/path.gmi     (relative to the base path's directory, with
                            "." / ".." normalized)
   A trailing '#fragment' on ref_in is stripped first (Gemini requests do
   not carry fragments). Returns 1 ok, 0 if ref_in cannot be resolved to a
   gemini:// target (e.g. it names another scheme). */
int gem_resolve_url(const char *base_host, int base_port, const char *base_path,
                    const char *ref_in,
                    char *out_host, int out_hostlen, int *out_port,
                    char *out_path, int out_pathlen)
{
    static char ref[GEM_URLBUF_MAX];
    static char tmp[GEM_URLBUF_MAX + 16];      /* "gemini:" + ref, see below */
    static char merged[GEM_URLBUF_MAX * 2];    /* worst case basepath+ref */
    static char normalized[GEM_URLBUF_MAX * 2];
    static char basepath_nq[GEM_URLBUF_MAX];
    static char refpath[GEM_URLBUF_MAX];
    static char refquery[GEM_URLBUF_MAX];
    char *hash;

    if (!ref_in || !out_host || !out_port || !out_path)
        return 0;

    strncpy(ref, ref_in, sizeof(ref) - 1);
    ref[sizeof(ref) - 1] = '\0';
    hash = strchr(ref, '#');
    if (hash) *hash = '\0';

    if (strstr(ref, "://") != NULL)
        return gem_parse_url(ref, out_host, out_hostlen, out_port,
                             out_path, out_pathlen);

    if (ref[0] == '/' && ref[1] == '/') {
        sprintf(tmp, "gemini:%s", ref);
        return gem_parse_url(tmp, out_host, out_hostlen, out_port,
                             out_path, out_pathlen);
    }

    strncpy(out_host, base_host, out_hostlen - 1);
    out_host[out_hostlen - 1] = '\0';
    *out_port = base_port;

    if (ref[0] == '\0') {
        strncpy(out_path, base_path, out_pathlen - 1);
        out_path[out_pathlen - 1] = '\0';
        return 1;
    }

    if (ref[0] == '?') {
        char *q;
        strncpy(basepath_nq, base_path, sizeof(basepath_nq) - 1);
        basepath_nq[sizeof(basepath_nq) - 1] = '\0';
        q = strchr(basepath_nq, '?');
        if (q) *q = '\0';
        sprintf(merged, "%s%s", basepath_nq, ref);
        strncpy(out_path, merged, out_pathlen - 1);
        out_path[out_pathlen - 1] = '\0';
        return 1;
    }

    {
        const char *qmark = strchr(ref, '?');
        int rl;

        refquery[0] = '\0';
        if (qmark) {
            rl = (int)(qmark - ref);
            if (rl > (int)sizeof(refpath) - 1) rl = (int)sizeof(refpath) - 1;
            memcpy(refpath, ref, rl);
            refpath[rl] = '\0';
            strncpy(refquery, qmark, sizeof(refquery) - 1);
            refquery[sizeof(refquery) - 1] = '\0';
        } else {
            strcpy(refpath, ref);
        }

        if (refpath[0] == '/') {
            gem_normalize_path(refpath, normalized, sizeof(normalized));
        } else {
            char *q2;
            strncpy(basepath_nq, base_path, sizeof(basepath_nq) - 1);
            basepath_nq[sizeof(basepath_nq) - 1] = '\0';
            q2 = strchr(basepath_nq, '?');
            if (q2) *q2 = '\0';
            gem_merge_path(basepath_nq, refpath, merged, sizeof(merged));
            gem_normalize_path(merged, normalized, sizeof(normalized));
        }

        {
            int nl = (int)strlen(normalized);
            if (nl > out_pathlen - 1) nl = out_pathlen - 1;
            memcpy(out_path, normalized, nl);
            out_path[nl] = '\0';
            if (refquery[0]) {
                int curl = (int)strlen(out_path);
                int ql = (int)strlen(refquery);
                if (curl + ql < out_pathlen) {
                    memcpy(out_path + curl, refquery, ql);
                    out_path[curl + ql] = '\0';
                }
            }
        }
    }
    return 1;
}

/* ---- text/gemini minimal renderer ---- */

/* One line, sans its terminator (a trailing \r is stripped too if the body
   uses CRLF line endings). Advances past the '\n'; if the buffer ends
   without one, returns a pointer at the terminating NUL (the next call then
   sees an empty string and the caller's `while (*p)` loop ends). */
static const char *gem_next_line(const char *p, char *out, int outlen)
{
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    if (len > outlen - 1) len = outlen - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    if (len > 0 && out[len - 1] == '\r') out[len - 1] = '\0';
    return nl ? nl + 1 : p + strlen(p);
}

static int gem_out_append(char *out, int outlen, int *o, const char *s)
{
    int l = (int)strlen(s);
    if (*o + l >= outlen) return 0;
    memcpy(out + *o, s, l);
    *o += l;
    out[*o] = '\0';
    return 1;
}

/* Minimally renders a text/gemini body into out: "#"/"##"/"###" headings,
   "=> url [label]" links (numbered so the reader has an index; the raw,
   unresolved ref is recorded into links[] in the same order), "* " list
   items, ">" quote lines, and "```" preformatted toggles (verbatim content,
   never reclassified while the toggle is active). Anything else is passed
   through unchanged. Returns the number of bytes written to out. */
int gem_render_gemtext(const char *body, char *out, int outlen,
                       gem_link_t *links, int maxlinks, int *nlinks)
{
    static char line[GEM_LINE_MAX];
    static char fmt[GEM_LINE_MAX + 128];
    const char *p = body;
    int o = 0;
    int preformat = 0;

    *nlinks = 0;
    out[0] = '\0';
    if (!body) return 0;

    while (*p) {
        p = gem_next_line(p, line, sizeof(line));

        if (line[0] == '`' && line[1] == '`' && line[2] == '`') {
            if (!preformat) {
                const char *alt = line + 3;
                while (*alt == ' ' || *alt == '\t') alt++;
                if (*alt)
                    sprintf(fmt, "---- pre: %s ----\n", alt);
                else
                    sprintf(fmt, "---- pre ----\n");
            } else {
                sprintf(fmt, "---- /pre ----\n");
            }
            preformat = !preformat;
            if (!gem_out_append(out, outlen, &o, fmt)) break;
            continue;
        }

        if (preformat) {
            sprintf(fmt, "%s\n", line);
            if (!gem_out_append(out, outlen, &o, fmt)) break;
            continue;
        }

        if (line[0] == '=' && line[1] == '>') {
            const char *q = line + 2;
            static char url[GEM_LINKURL_MAX];
            const char *label;
            int ul = 0;

            while (*q == ' ' || *q == '\t') q++;
            while (*q && *q != ' ' && *q != '\t' && ul < (int)sizeof(url) - 1)
                url[ul++] = *q++;
            url[ul] = '\0';
            while (*q == ' ' || *q == '\t') q++;
            label = *q ? q : url;

            if (ul > 0) {
                int idx = *nlinks;
                if (idx < maxlinks) {
                    strncpy(links[idx].ref, url, sizeof(links[idx].ref) - 1);
                    links[idx].ref[sizeof(links[idx].ref) - 1] = '\0';
                    (*nlinks)++;
                }
                sprintf(fmt, "[%d] %s\n", idx + 1, label);
                if (!gem_out_append(out, outlen, &o, fmt)) break;
            }
            continue;
        }

        if (line[0] == '#') {
            int level = 0;
            const char *t = line;
            while (*t == '#' && level < 3) { level++; t++; }
            while (*t == '#') t++;
            while (*t == ' ' || *t == '\t') t++;

            if (level == 1)
                sprintf(fmt, "\n=== %s ===\n", t);
            else if (level == 2)
                sprintf(fmt, "\n-- %s --\n", t);
            else
                sprintf(fmt, "  . %s\n", t);
            if (!gem_out_append(out, outlen, &o, fmt)) break;
            continue;
        }

        if (line[0] == '*' && (line[1] == ' ' || line[1] == '\t' || line[1] == '\0')) {
            const char *t = line + 1;
            while (*t == ' ' || *t == '\t') t++;
            sprintf(fmt, "  * %s\n", t);
            if (!gem_out_append(out, outlen, &o, fmt)) break;
            continue;
        }

        if (line[0] == '>') {
            const char *t = line + 1;
            while (*t == ' ' || *t == '\t') t++;
            sprintf(fmt, "  | %s\n", t);
            if (!gem_out_append(out, outlen, &o, fmt)) break;
            continue;
        }

        sprintf(fmt, "%s\n", line);
        if (!gem_out_append(out, outlen, &o, fmt)) break;
    }

    return o;
}

/* ====================================================================== */
/* Everything below is platform code (real on Amiga, stubbed on host)     */
/* ====================================================================== */

#ifndef HOST_TEST

static long rc_atol(const char *s)
{
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    return neg ? -(long)rc_strtoul(s, NULL, 10) : (long)rc_strtoul(s, NULL, 10);
}

/* ---- config (set from argv) ---- */
static int         g_use_proxy    = 0;
static char        g_proxy_host[128] = "127.0.0.1";
static int         g_proxy_port   = 8791;
static int         g_strict_tls   = 0;   /* off: Gemini capsules are almost
                                             always self-signed; see the
                                             file header note on TOFU */
static int         g_no_interactive = 0;
static int         g_verbose      = 0;
static const char *g_output_file  = NULL;

/* link table + big scratch buffers for the current page: static so none of
   this lands on the Amiga's (small, easy to overrun) CLI stack */
static gem_link_t  g_links[GEM_MAX_LINKS];
static int         g_nlinks = 0;
static char        g_resp[GEM_RESPBUF];
static char        g_outbuf[GEM_OUTBUF];
static char        g_reqbuf[GEM_URLBUF_MAX];
static char        g_reqline[GEM_URLBUF_MAX + 4];
static char        g_scratch_host[GEM_HOSTBUF];
static char        g_scratch_path[GEM_PATHBUF];

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

#ifndef NO_AMISSL
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#endif

/* SocketBase is owned by the vendored rtc_common.c */
extern struct Library *SocketBase;

#ifndef NO_AMISSL
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *UtilityBase = NULL;
static int g_amissl_errno = 0;
static int g_amissl_ready = 0;
#endif

/* ---- small helpers ---- */

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
    sa.sin_port = (unsigned short)port;  /* m68k big endian: htons is identity */

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            fprintf(stderr, "[gemini] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "[gemini] socket() failed\n");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[gemini] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* ---- AmiSSL init (follows SDK example https.c, same as claude.c) ---- */

#ifdef NO_AMISSL
static void amissl_cleanup(void) { }
static long amissl_fetch(const char *host, int port, const char *reqline,
                         char *resp, long resplen)
{
    (void)host; (void)port; (void)reqline; (void)resp; (void)resplen;
    fprintf(stderr, "[gemini] built without AmiSSL; use --proxy host:port\n");
    return -1;
}
#else
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

    if (!rtc_net_open())
        return 0;

    UtilityBase = OpenLibrary((STRPTR)"utility.library", 0);
    if (!UtilityBase) {
        fprintf(stderr, "[gemini] cannot open utility.library\n");
        return 0;
    }
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(stderr, "[gemini] cannot open amisslmaster.library v%d\n"
                        "         install AmiSSL, or use --proxy host:port\n",
                        AMISSLMASTER_MIN_VERSION);
        amissl_cleanup();
        return 0;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        fprintf(stderr, "[gemini] AmiSSL version is too old\n");
        amissl_cleanup();
        return 0;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        fprintf(stderr, "[gemini] cannot open amissl.library\n");
        amissl_cleanup();
        return 0;
    }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&g_amissl_errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        fprintf(stderr, "[gemini] InitAmiSSL failed\n");
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

/* ---- AmiSSL direct TLS transport (primary) ----
   Sends reqline (the request URL + "\r\n") and reads until the server
   closes the connection -- Gemini has no length framing on the response. */
static long amissl_fetch(const char *host, int port, const char *reqline,
                         char *resp, long resplen)
{
    SSL_CTX *ctx;
    SSL *ssl;
    long sock, got, total = 0;
    int rc;

    if (!amissl_init())
        return -1;
    seed_rand();

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fprintf(stderr, "[gemini] SSL_CTX_new failed\n");
        return -1;
    }
    if (g_strict_tls) {
        SSL_CTX_set_default_verify_paths(ctx);
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    ssl = SSL_new(ctx);
    if (!ssl) {
        fprintf(stderr, "[gemini] SSL_new failed\n");
        SSL_CTX_free(ctx);
        return -1;
    }

    sock = tcp_connect(host, port);
    if (sock < 0) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, host);

    rc = SSL_connect(ssl);
    if (rc <= 0) {
        fprintf(stderr, "[gemini] TLS handshake to %s failed", host);
        if (g_strict_tls) {
            long vr = SSL_get_verify_result(ssl);
            if (vr != X509_V_OK)
                fprintf(stderr, " (cert verify: %s; most Gemini capsules use "
                                "self-signed certs -- drop --strict-tls)",
                        X509_verify_cert_error_string(vr));
        }
        fprintf(stderr, "\n");
        CloseSocket(sock);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return -1;
    }

    if (SSL_write(ssl, reqline, (int)strlen(reqline)) <= 0) {
        fprintf(stderr, "[gemini] SSL_write failed\n");
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
#endif /* NO_AMISSL */

/* ---- plain-TCP proxy transport (fallback, see proxy/gemini_amiga_proxy.py) ----
   Wire format to the proxy: "<target host> <target port>\n" then the exact
   Gemini request line, then read the raw relayed response until close. */
static long proxy_fetch(const char *proxy_host, int proxy_port,
                        const char *target_host, int target_port,
                        const char *reqline, char *resp, long resplen)
{
    static char hdr[300];
    long s, got, total = 0;
    int hl, rll;

    if (!rtc_net_open())
        return -1;

    hl = sprintf(hdr, "%s %d\n", target_host, target_port);
    if (hl < 0 || hl >= (int)sizeof(hdr)) {
        fprintf(stderr, "[gemini] proxy header too large\n");
        return -1;
    }

    if (g_verbose)
        printf("[gemini] proxy connecting to %s:%d for %s:%d\n",
              proxy_host, proxy_port, target_host, target_port);
    s = tcp_connect(proxy_host, proxy_port);
    if (s < 0)
        return -1;

    if (send(s, hdr, hl, 0) != hl) {
        fprintf(stderr, "[gemini] proxy header send failed\n");
        CloseSocket(s);
        return -1;
    }
    rll = (int)strlen(reqline);
    if (send(s, (char *)reqline, rll, 0) != rll) {
        fprintf(stderr, "[gemini] proxy request send failed\n");
        CloseSocket(s);
        return -1;
    }

    while (total < resplen - 1) {
        got = recv(s, resp + total, resplen - 1 - total, 0);
        if (got <= 0)
            break;
        total += got;
        if (rtc_check_break())
            break;
    }
    resp[total] = '\0';
    CloseSocket(s);
    return total;
}

static long transport_fetch(const char *host, int port, const char *reqline,
                            char *resp, long resplen)
{
    if (g_use_proxy)
        return proxy_fetch(g_proxy_host, g_proxy_port, host, port, reqline,
                           resp, resplen);
    return amissl_fetch(host, port, reqline, resp, resplen);
}

/* ---- save a response body to an Amiga file (BPTR, not libnix stdio --
   Write() takes an explicit length so this is correct even for a body
   with embedded NUL bytes; only the text-rendering paths below assume a
   NUL-terminated C string) ---- */
static void save_body_to_file(const char *path, const char *body, long len)
{
    BPTR fh;
    long w;

    fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) {
        fprintf(stderr, "[gemini] cannot open %s for writing\n", path);
        return;
    }
    w = Write(fh, (APTR)body, len);
    Close(fh);
    if (w != len)
        fprintf(stderr, "[gemini] wrote %ld of %ld bytes to %s\n", w, len, path);
}

static void usage(void)
{
    printf("gemini - a Gemini protocol client/browser for AmigaOS\n\n");
    printf("Usage:\n");
    printf("  gemini gemini://host[:port]/path\n\n");
    printf("Options:\n");
    printf("  --proxy host:port     use the plain-TCP host proxy (no AmiSSL needed;\n");
    printf("                        see proxy/gemini_amiga_proxy.py)\n");
    printf("  --strict-tls          verify the server's TLS certificate (off by\n");
    printf("                        default: Gemini capsules are almost always\n");
    printf("                        self-signed, see README.md)\n");
    printf("  -o, --output FILE     save the response body to FILE instead of\n");
    printf("                        rendering it\n");
    printf("  -1, --no-interactive  fetch once, print, and exit (no link prompt)\n");
    printf("  -v, --verbose         print transport progress\n");
    printf("  -h, --help            this help\n\n");
    printf("At the \"gemini>\" prompt: enter a link number, a gemini:// URL or a\n");
    printf("relative reference, or leave it blank (or type q) to quit.\n");
}

/* One browsing session, starting at (host, port, path). Fetches, handles
   INPUT/SUCCESS/REDIRECT/FAILURE per the Gemini status categories, renders
   text/gemini bodies, and (unless --no-interactive) prompts for the next
   link after each page. Loops until the user quits or stdin is closed. */
static void run_browser(const char *start_host, int start_port,
                        const char *start_path)
{
    static char host[GEM_HOSTBUF];
    static char path[GEM_PATHBUF];
    int port;

    strncpy(host, start_host, sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';
    strncpy(path, start_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    port = start_port;

    for (;;) {
        int redirects = 0;

        for (;;) {   /* redirect/input inner loop, bounded */
            static char meta[1024];
            const char *body;
            long n;
            int code;

            if (gem_build_url(host, port, path, g_reqbuf, GEM_URLBUF_MAX) < 0) {
                fprintf(stderr, "[gemini] URL too long\n");
                break;
            }
            sprintf(g_reqline, "%s\r\n", g_reqbuf);
            if (g_verbose) {
                printf("[gemini] -> %s", g_reqline);
                fflush(stdout);
            }
            n = transport_fetch(host, port, g_reqline, g_resp, sizeof(g_resp));
            if (n < 0) {
                fprintf(stderr, "[gemini] could not fetch %s\n", g_reqbuf);
                break;
            }

            code = gem_status_code(g_resp);
            if (code < 0 || !gem_status_meta(g_resp, meta, sizeof(meta))) {
                fprintf(stderr, "[gemini] malformed response from %s\n", g_reqbuf);
                break;
            }
            if (g_verbose)
                printf("[gemini] <- %d %s\n", code, meta);

            if (code >= 10 && code <= 19) {
                static char inbuf[512];
                static char enc[1024];
                char *q;

                printf("%s\n%s: ", g_reqbuf, meta);
                fflush(stdout);
                if (!fgets(inbuf, sizeof(inbuf), stdin))
                    break;
                {
                    int l = (int)strlen(inbuf);
                    while (l > 0 && (inbuf[l - 1] == '\n' || inbuf[l - 1] == '\r'))
                        inbuf[--l] = '\0';
                }
                gem_url_encode(inbuf, enc, sizeof(enc));
                q = strchr(path, '?');
                if (q) *q = '\0';
                {
                    static char newpath[GEM_PATHBUF];
                    sprintf(newpath, "%.*s?%s",
                           (int)sizeof(newpath) - 16, path, enc);
                    strncpy(path, newpath, sizeof(path) - 1);
                    path[sizeof(path) - 1] = '\0';
                }
                redirects++;
                if (redirects > 5) {
                    fprintf(stderr, "[gemini] too many input round-trips\n");
                    break;
                }
                continue;
            }

            if (code >= 20 && code <= 29) {
                long bodylen;

                body = gem_body(g_resp);
                bodylen = body ? (n - (long)(body - g_resp)) : 0;
                if (bodylen < 0) bodylen = 0;

                if (g_output_file) {
                    save_body_to_file(g_output_file, body ? body : "", bodylen);
                    printf("[gemini] saved %ld bytes to %s (%s)\n",
                          bodylen, g_output_file, meta);
                } else if (strncmp(meta, "text/gemini", 11) == 0) {
                    gem_render_gemtext(body ? body : "", g_outbuf,
                                       sizeof(g_outbuf), g_links,
                                       GEM_MAX_LINKS, &g_nlinks);
                    printf("%s", g_outbuf);
                } else if (strncmp(meta, "text/", 5) == 0) {
                    printf("%s\n", body ? body : "");
                    g_nlinks = 0;
                } else {
                    printf("[gemini] %s, %ld bytes (not text; use -o FILE to save)\n",
                          meta, bodylen);
                    g_nlinks = 0;
                }
                fflush(stdout);
                break;
            }

            if (code >= 30 && code <= 39) {
                int nport;

                redirects++;
                if (redirects > 5) {
                    fprintf(stderr, "[gemini] too many redirects (stopped at %s)\n",
                            meta);
                    break;
                }
                if (!gem_resolve_url(host, port, path, meta,
                                     g_scratch_host, sizeof(g_scratch_host),
                                     &nport, g_scratch_path, sizeof(g_scratch_path))) {
                    fprintf(stderr, "[gemini] cannot follow redirect to %s\n", meta);
                    break;
                }
                {
                    static char shown[GEM_URLBUF_MAX];
                    gem_build_url(g_scratch_host, nport, g_scratch_path,
                                 shown, sizeof(shown));
                    printf("[gemini] -> redirect to %s\n", shown);
                }
                strcpy(host, g_scratch_host);
                port = nport;
                strcpy(path, g_scratch_path);
                continue;
            }

            /* 40-69: temporary/permanent failure, client cert required */
            fprintf(stderr, "[gemini] %d %s\n", code, meta);
            break;
        }

        if (g_no_interactive)
            return;

        for (;;) {
            static char line[600];

            printf("\ngemini> ");
            fflush(stdout);
            if (!fgets(line, sizeof(line), stdin))
                return;
            {
                int l = (int)strlen(line);
                while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
                    line[--l] = '\0';
            }
            if (line[0] == '\0')
                return;
            if (strcmp(line, "q") == 0 || strcmp(line, "quit") == 0)
                return;

            if (gem_is_digit(line[0])) {
                int idx = (int)rc_atol(line) - 1;
                int nport;

                if (idx < 0 || idx >= g_nlinks) {
                    printf("no such link: %s\n", line);
                    continue;
                }
                if (!gem_resolve_url(host, port, path, g_links[idx].ref,
                                     g_scratch_host, sizeof(g_scratch_host),
                                     &nport, g_scratch_path, sizeof(g_scratch_path))) {
                    printf("cannot follow that link (unsupported scheme)\n");
                    continue;
                }
                strcpy(host, g_scratch_host);
                port = nport;
                strcpy(path, g_scratch_path);
                break;
            }

            {
                int nport;
                if (!gem_resolve_url(host, port, path, line,
                                     g_scratch_host, sizeof(g_scratch_host),
                                     &nport, g_scratch_path, sizeof(g_scratch_path))) {
                    printf("cannot open that (only gemini:// is supported)\n");
                    continue;
                }
                strcpy(host, g_scratch_host);
                port = nport;
                strcpy(path, g_scratch_path);
            }
            break;
        }
    }
}

int main(int argc, char **argv)
{
    static char host[GEM_HOSTBUF];
    static char path[GEM_PATHBUF];
    const char *url = NULL;
    int port;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            static char hp[160];
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
        } else if (strcmp(argv[i], "--strict-tls") == 0) {
            g_strict_tls = 1;
        } else if (strcmp(argv[i], "-1") == 0 ||
                  strcmp(argv[i], "--no-interactive") == 0) {
            g_no_interactive = 1;
        } else if ((strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            g_output_file = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] != '-') {
            url = argv[i];
        } else {
            fprintf(stderr, "[gemini] unknown option %s\n", argv[i]);
            usage();
            return 20;
        }
    }

    if (!url) {
        usage();
        return 5;
    }

    if (!gem_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        fprintf(stderr, "[gemini] bad or unsupported URL: %s\n", url);
        fprintf(stderr, "         (only gemini:// URLs are supported)\n");
        return 20;
    }

    if (!rtc_net_open())
        return 20;

    run_browser(host, port, path);

    amissl_cleanup();
    rtc_net_close();
    return 0;
}

#else /* ================= HOST_TEST: pure-function self-test ============= */

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    printf("gemini.c host self-test\n");
    printf("========================\n");

    /* 1. URL parsing */
    {
        char host[256], path[1024];
        int port;

        check("gemini://host/ parses",
              gem_parse_url("gemini://example.org/", host, sizeof(host),
                            &port, path, sizeof(path)));
        check("  host", strcmp(host, "example.org") == 0);
        check("  default port 1965", port == 1965);
        check("  path /", strcmp(path, "/") == 0);

        check("gemini://host (no slash) parses",
              gem_parse_url("gemini://example.org", host, sizeof(host),
                            &port, path, sizeof(path)));
        check("  defaults path to /", strcmp(path, "/") == 0);

        check("gemini://host:port/path parses",
              gem_parse_url("gemini://example.org:1966/foo/bar", host,
                            sizeof(host), &port, path, sizeof(path)));
        check("  custom port", port == 1966);
        check("  path", strcmp(path, "/foo/bar") == 0);

        check("query string kept in path",
              gem_parse_url("gemini://example.org/foo?q=1", host,
                            sizeof(host), &port, path, sizeof(path)));
        check("  path+query", strcmp(path, "/foo?q=1") == 0);

        check("host-only query (no path) gets a leading /",
              gem_parse_url("gemini://example.org?q=1", host,
                            sizeof(host), &port, path, sizeof(path)));
        check("  /?q=1", strcmp(path, "/?q=1") == 0);

        check("bare host (no scheme) is accepted",
              gem_parse_url("example.org/bare", host, sizeof(host),
                            &port, path, sizeof(path)));
        check("  host", strcmp(host, "example.org") == 0);
        check("  path", strcmp(path, "/bare") == 0);

        check("https:// is rejected (not our scheme)",
              !gem_parse_url("https://example.org/", host, sizeof(host),
                             &port, path, sizeof(path)));

        check("gemini:// with no host is rejected",
              !gem_parse_url("gemini://", host, sizeof(host),
                             &port, path, sizeof(path)));

        check("fragment is stripped from the path",
              gem_parse_url("gemini://example.org/page#frag", host,
                            sizeof(host), &port, path, sizeof(path)));
        check("  path without #frag", strcmp(path, "/page") == 0);

        check("build_url round-trips (default port omitted)",
              gem_parse_url("gemini://example.org/x", host, sizeof(host),
                            &port, path, sizeof(path)));
        {
            char built[256];
            gem_build_url(host, port, path, built, sizeof(built));
            check("  gemini://example.org/x",
                  strcmp(built, "gemini://example.org/x") == 0);
        }
        {
            char built[256];
            gem_build_url("example.org", 1966, "/x", built, sizeof(built));
            check("build_url keeps a non-default port",
                  strcmp(built, "gemini://example.org:1966/x") == 0);
        }
    }

    /* 2. status line parsing */
    {
        int code;
        char meta[128];
        const char *body;

        code = gem_status_code("20 text/gemini\r\nHello");
        check("status code 20 parsed", code == 20);
        check("meta parsed ok",
              gem_status_meta("20 text/gemini\r\nHello", meta, sizeof(meta)));
        check("meta text", strcmp(meta, "text/gemini") == 0);
        body = gem_body("20 text/gemini\r\nHello");
        check("body pointer correct", body && strcmp(body, "Hello") == 0);

        code = gem_status_code("51 not found\r\n");
        check("status code 51 parsed", code == 51);
        check("meta text 'not found'",
              gem_status_meta("51 not found\r\n", meta, sizeof(meta)) &&
              strcmp(meta, "not found") == 0);
        body = gem_body("51 not found\r\n");
        check("empty body after header-only response",
              body && strcmp(body, "") == 0);

        check("bad status code (non-digit) rejected",
              gem_status_code("ab text\r\n") == -1);
        check("missing mandatory space rejected",
              !gem_status_meta("20text/gemini\r\n", meta, sizeof(meta)));

        check("lenient bare-LF meta still parses",
              gem_status_meta("20 text/gemini\nHello", meta, sizeof(meta)) &&
              strcmp(meta, "text/gemini") == 0);
        body = gem_body("20 text/gemini\nHello");
        check("lenient bare-LF body pointer",
              body && strcmp(body, "Hello") == 0);
    }

    /* 3. percent encode/decode round trip */
    {
        char enc[256], dec[256];
        const char *orig = "hello world/2?x=y&z=1";

        check("encode succeeds", gem_url_encode(orig, enc, sizeof(enc)) > 0);
        check("encoded string has no raw space",
              strchr(enc, ' ') == NULL);
        check("decode round-trips", gem_url_decode(enc, dec, sizeof(dec)) > 0);
        check("decoded matches original", strcmp(dec, orig) == 0);
    }

    /* 4. relative URL resolution */
    {
        char host[256], path[1024];
        int port;
        const char *bhost = "example.org";
        int bport = 1965;
        const char *bpath = "/dir/page.gmi";

        check("plain relative ref merges into the base directory",
              gem_resolve_url(bhost, bport, bpath, "other.gmi",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  /dir/other.gmi", strcmp(path, "/dir/other.gmi") == 0);
        check("  same host", strcmp(host, "example.org") == 0);

        check("absolute-path ref replaces the whole path",
              gem_resolve_url(bhost, bport, bpath, "/abs.gmi",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  /abs.gmi", strcmp(path, "/abs.gmi") == 0);

        check("dot-dot ref climbs out of the directory",
              gem_resolve_url(bhost, bport, bpath, "../up.gmi",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  /up.gmi", strcmp(path, "/up.gmi") == 0);

        check("absolute gemini:// ref wins outright",
              gem_resolve_url(bhost, bport, bpath, "gemini://other.org/x",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  other.org", strcmp(host, "other.org") == 0);
        check("  /x", strcmp(path, "/x") == 0);

        check("scheme-relative // ref resolves like gemini://",
              gem_resolve_url(bhost, bport, bpath, "//other.org/x",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  other.org (scheme-relative)", strcmp(host, "other.org") == 0);

        check("query-only ref keeps the path",
              gem_resolve_url(bhost, bport, bpath, "?q=1",
                              host, sizeof(host), &port, path, sizeof(path)));
        check("  /dir/page.gmi?q=1", strcmp(path, "/dir/page.gmi?q=1") == 0);

        check("a non-gemini absolute scheme cannot be resolved",
              !gem_resolve_url(bhost, bport, bpath, "https://example.com/",
                               host, sizeof(host), &port, path, sizeof(path)));
    }

    /* 5. text/gemini rendering */
    {
        static const char *fixture =
            "# Title One\n"
            "Some intro text.\n"
            "\n"
            "## Section\n"
            "* item one\n"
            "* item two\n"
            "\n"
            "=> gemini://example.org/about About page\n"
            "=> /relative.gmi\n"
            "=> https://example.com/ external (not gemini)\n"
            "\n"
            "> A quote line\n"
            "\n"
            "```alt text for code\n"
            "* not a list, this is code\n"
            "```\n"
            "### Sub heading\n";
        char out[4096];
        gem_link_t links[8];
        int nlinks = 0;

        gem_render_gemtext(fixture, out, sizeof(out), links, 8, &nlinks);

        check("three links found", nlinks == 3);
        check("link 0 ref", strcmp(links[0].ref, "gemini://example.org/about") == 0);
        check("link 1 ref (bare relative)", strcmp(links[1].ref, "/relative.gmi") == 0);
        check("link 2 ref (non-gemini)", strcmp(links[2].ref, "https://example.com/") == 0);

        check("link 1 labeled and numbered", strstr(out, "[1] About page") != NULL);
        check("link 2 falls back to the url as its label",
              strstr(out, "[2] /relative.gmi") != NULL);
        check("link 3 labeled", strstr(out, "[3] external (not gemini)") != NULL);

        check("H1 rendered", strstr(out, "=== Title One ===") != NULL);
        check("H2 rendered", strstr(out, "-- Section --") != NULL);
        check("H3 rendered", strstr(out, "  . Sub heading") != NULL);

        check("list item one", strstr(out, "  * item one") != NULL);
        check("list item two", strstr(out, "  * item two") != NULL);
        check("quote line", strstr(out, "  | A quote line") != NULL);

        check("pre-block open marker with alt text",
              strstr(out, "---- pre: alt text for code ----") != NULL);
        check("pre-block close marker",
              strstr(out, "---- /pre ----") != NULL);
        check("content inside pre is verbatim, not reclassified as a list",
              strstr(out, "* not a list, this is code") != NULL);
        check("the verbatim pre line is NOT bulleted",
              strstr(out, "  * not a list, this is code") == NULL);
    }

    printf("========================\n");
    if (failures == 0)
        printf("ALL CHECKS PASSED (0 failures)\n");
    else
        printf("%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}

#endif /* HOST_TEST */
