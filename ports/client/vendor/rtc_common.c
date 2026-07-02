/*
 * rtc_common.c - vendored copy for the amiport client.
 *
 * ORIGIN: copied from tools/common/rtc_common.c (which itself derives
 * from miner/rustchain_amiga_miner.c) on 2026-07-02. See rtc_common.h
 * for the list of modifications. SHA-1, the JSON scan, URL parsing and
 * the request formatting are verbatim from the original; the socket
 * layer is refactored behind four small helpers so the HOST_TEST build
 * can use real POSIX sockets, and rtc_http_get_file() is new.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_common.h"

#ifndef HOST_TEST

#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Library *SocketBase = NULL;

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

extern struct ExecBase *SysBase;

#else /* HOST_TEST: real POSIX sockets */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#endif

/* ------------------------------------------------------------------ */
/* SHA-1 (verbatim from the miner)                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long h[5];
    unsigned long len_lo;
    unsigned long len_hi;
    unsigned char buf[64];
    int buf_used;
} sha1_ctx;

static unsigned long sha1_rol(unsigned long v, int n)
{
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFFUL;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->buf_used = 0;
}

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    unsigned long w[80];
    unsigned long a, b, d, e, f, k, t;
    unsigned long cc;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)p[i * 4] << 24) |
               ((unsigned long)p[i * 4 + 1] << 16) |
               ((unsigned long)p[i * 4 + 2] << 8) |
               ((unsigned long)p[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b) & d);          k = 0x5A827999UL; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1UL; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCUL; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6UL; }
        t = (sha1_rol(a, 5) + f + e + k + w[i]) & 0xFFFFFFFFUL;
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
    }

    c->h[0] = (c->h[0] + a) & 0xFFFFFFFFUL;
    c->h[1] = (c->h[1] + b) & 0xFFFFFFFFUL;
    c->h[2] = (c->h[2] + cc) & 0xFFFFFFFFUL;
    c->h[3] = (c->h[3] + d) & 0xFFFFFFFFUL;
    c->h[4] = (c->h[4] + e) & 0xFFFFFFFFUL;
}

static void sha1_update(sha1_ctx *c, const unsigned char *p, unsigned long n)
{
    unsigned long old = c->len_lo;
    c->len_lo = (c->len_lo + (n << 3)) & 0xFFFFFFFFUL;
    if (c->len_lo < old)
        c->len_hi++;
    c->len_hi += (n >> 29);

    while (n > 0) {
        unsigned long take = 64 - c->buf_used;
        if (take > n) take = n;
        memcpy(c->buf + c->buf_used, p, take);
        c->buf_used += (int)take;
        p += take;
        n -= take;
        if (c->buf_used == 64) {
            sha1_block(c, c->buf);
            c->buf_used = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenb[8];
    unsigned long lo = c->len_lo, hi = c->len_hi;
    int i;

    sha1_update(c, &pad, 1);
    while (c->buf_used != 56)
        sha1_update(c, &zero, 1);

    lenb[0] = (unsigned char)(hi >> 24); lenb[1] = (unsigned char)(hi >> 16);
    lenb[2] = (unsigned char)(hi >> 8);  lenb[3] = (unsigned char)(hi);
    lenb[4] = (unsigned char)(lo >> 24); lenb[5] = (unsigned char)(lo >> 16);
    lenb[6] = (unsigned char)(lo >> 8);  lenb[7] = (unsigned char)(lo);
    /* length goes in raw, do not recount it */
    memcpy(c->buf + 56, lenb, 8);
    sha1_block(c, c->buf);

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

static void sha1_hex_digest(const unsigned char dig[20], char out[41])
{
    static const char hexd[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 20; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0x0F];
    }
    out[40] = '\0';
}

void rtc_sha1_hex(const unsigned char *data, unsigned long n, char out[41])
{
    sha1_ctx c;
    unsigned char dig[20];

    sha1_init(&c);
    sha1_update(&c, data, n);
    sha1_final(&c, dig);
    sha1_hex_digest(dig, out);
}

/* ------------------------------------------------------------------ */
/* JSON scan                                                          */
/* ------------------------------------------------------------------ */

int rtc_json_raw(const char *json, const char *key, char *out, int outlen)
{
    char pat[64];
    const char *p;
    int o = 0;

    if ((int)strlen(key) > 60 || outlen < 2) {
        if (outlen > 0) out[0] = '\0';
        return 0;
    }
    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        out[0] = '\0';
        return 0;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o < outlen - 1) {
            if (*p == '\\' && p[1])
                p++; /* keep the escaped character, drop the backslash */
            out[o++] = *p++;
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

/* ------------------------------------------------------------------ */
/* HTTP formatting / parsing                                          */
/* ------------------------------------------------------------------ */

int rtc_format_get(char *out, int outlen, const char *host, int port,
                   const char *path)
{
    int n;

    if ((int)(strlen(host) + strlen(path)) > outlen - 128)
        return -1;
    n = sprintf(out,
        "GET %s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: amiport-amiga/" RTC_TOOLS_VERSION "\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);
    if (n >= outlen)
        return -1;
    return n;
}

int rtc_http_status(const char *resp)
{
    if (strncmp(resp, "HTTP/1.", 7) != 0)
        return -1;
    return atoi(resp + 9);
}

const char *rtc_http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    if (p)
        return p + 4;
    p = strstr(resp, "\n\n");
    if (p)
        return p + 2;
    return NULL;
}

int rtc_parse_url(const char *url, char *host, int hostlen, int *port,
                  char *path, int pathlen)
{
    const char *p, *slash, *colon;
    int hlen;

    if (strncmp(url, "http://", 7) != 0)
        return 0;
    p = url + 7;
    if (*p == '\0' || *p == '/' || *p == ':')
        return 0;

    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && slash && colon > slash)
        colon = NULL; /* colon inside the path, not a port */

    hlen = (int)((colon ? colon : (slash ? slash : p + strlen(p))) - p);
    if (hlen <= 0 || hlen >= hostlen)
        return 0;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = 80;
    if (colon) {
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535)
            return 0;
    }

    if (slash) {
        if ((int)strlen(slash) >= pathlen)
            return 0;
        strcpy(path, slash);
    } else {
        if (pathlen < 2)
            return 0;
        strcpy(path, "/");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* socket layer: four helpers, two implementations                    */
/* ------------------------------------------------------------------ */

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int i = 0;
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

#ifndef HOST_TEST

int rtc_net_open(void)
{
    if (SocketBase)
        return 1;
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        fprintf(stderr, "[FAIL] bsdsocket.library not available\n");
        fprintf(stderr, "       start a TCP/IP stack (Roadshow/AmiTCP) or enable\n");
        fprintf(stderr, "       bsdsocket_library = 1 in FS-UAE\n");
        return 0;
    }
    return 1;
}

void rtc_net_close(void)
{
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

static long net_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    unsigned long ip;
    long s;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (unsigned short)port; /* m68k is big endian, htons is identity */

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            fprintf(stderr, "[FAIL] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "[FAIL] socket() failed\n");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[FAIL] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }
    return s;
}

static long net_send(long s, const char *p, long n)
{
    return send(s, (void *)p, n, 0);
}

static long net_recv(long s, char *p, long n)
{
    return recv(s, p, n, 0);
}

static void net_shut(long s)
{
    CloseSocket(s);
}

int rtc_check_break(void)
{
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

#else /* HOST_TEST: POSIX */

int rtc_net_open(void)
{
    return 1;
}

void rtc_net_close(void)
{
}

static long net_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    unsigned long ip;
    long s;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = htonl(ip);
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "[FAIL] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "[FAIL] socket() failed\n");
        return -1;
    }
    if (connect((int)s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[FAIL] connect to %s:%d failed\n", host, port);
        close((int)s);
        return -1;
    }
    return s;
}

static long net_send(long s, const char *p, long n)
{
    return (long)send((int)s, p, (size_t)n, 0);
}

static long net_recv(long s, char *p, long n)
{
    return (long)recv((int)s, p, (size_t)n, 0);
}

static void net_shut(long s)
{
    close((int)s);
}

int rtc_check_break(void)
{
    return 0;
}

#endif

/* ------------------------------------------------------------------ */
/* HTTP transfers (shared implementation)                             */
/* ------------------------------------------------------------------ */

long rtc_http_get(const char *host, int port, const char *path,
                  char *resp, long resplen)
{
    static char req[1024];
    long s;
    long got, total = 0;
    int reqlen;

    reqlen = rtc_format_get(req, sizeof(req), host, port, path);
    if (reqlen < 0) {
        fprintf(stderr, "[FAIL] request too large\n");
        return -1;
    }

    s = net_connect(host, port);
    if (s < 0)
        return -1;

    if (net_send(s, req, reqlen) != reqlen) {
        fprintf(stderr, "[FAIL] send failed\n");
        net_shut(s);
        return -1;
    }

    while (total < resplen - 1) {
        got = net_recv(s, resp + total, resplen - 1 - total);
        if (got <= 0)
            break;
        total += got;
        if (rtc_check_break())
            break;
    }
    resp[total] = '\0';
    net_shut(s);

    return total;
}

int rtc_http_get_file(const char *host, int port, const char *path,
                      FILE *out, long *bodylen, char sha1hex[41])
{
    static char req[1024];
    static char hdr[8192];
    static char chunk[4096];
    sha1_ctx ctx;
    unsigned char dig[20];
    const char *body = NULL;
    long s, got;
    long hlen = 0, n;
    int reqlen, status;

    *bodylen = 0;
    sha1hex[0] = '\0';

    reqlen = rtc_format_get(req, sizeof(req), host, port, path);
    if (reqlen < 0) {
        fprintf(stderr, "[FAIL] request too large\n");
        return -1;
    }

    s = net_connect(host, port);
    if (s < 0)
        return -1;

    if (net_send(s, req, reqlen) != reqlen) {
        fprintf(stderr, "[FAIL] send failed\n");
        net_shut(s);
        return -1;
    }

    /* accumulate until the blank line that ends the headers */
    for (;;) {
        hdr[hlen] = '\0';
        body = rtc_http_body(hdr);
        if (body)
            break;
        if (hlen >= (long)sizeof(hdr) - 1) {
            fprintf(stderr, "[FAIL] response headers too large\n");
            net_shut(s);
            return -1;
        }
        got = net_recv(s, hdr + hlen, (long)sizeof(hdr) - 1 - hlen);
        if (got <= 0) {
            fprintf(stderr, "[FAIL] connection closed before headers ended\n");
            net_shut(s);
            return -1;
        }
        hlen += got;
    }

    status = rtc_http_status(hdr);
    if (status < 0) {
        fprintf(stderr, "[FAIL] reply is not HTTP\n");
        net_shut(s);
        return -1;
    }

    sha1_init(&ctx);

    /* body bytes that arrived with the header chunk */
    n = hlen - (long)(body - hdr);
    if (n > 0) {
        if (fwrite(body, 1, (size_t)n, out) != (size_t)n) {
            fprintf(stderr, "[FAIL] short write to output file\n");
            net_shut(s);
            return -1;
        }
        sha1_update(&ctx, (const unsigned char *)body, (unsigned long)n);
        *bodylen += n;
    }

    /* stream the rest straight to disk */
    for (;;) {
        got = net_recv(s, chunk, (long)sizeof(chunk));
        if (got <= 0)
            break;
        if (fwrite(chunk, 1, (size_t)got, out) != (size_t)got) {
            fprintf(stderr, "[FAIL] short write to output file\n");
            net_shut(s);
            return -1;
        }
        sha1_update(&ctx, (const unsigned char *)chunk, (unsigned long)got);
        *bodylen += got;
        if (rtc_check_break()) {
            fprintf(stderr, "[FAIL] break received, download aborted\n");
            net_shut(s);
            return -1;
        }
    }

    net_shut(s);
    sha1_final(&ctx, dig);
    sha1_hex_digest(dig, sha1hex);
    return status;
}
