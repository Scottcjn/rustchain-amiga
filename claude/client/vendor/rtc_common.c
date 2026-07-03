/*
 * rtc_common.c - shared helpers for the RustChain Amiga tools.
 *
 * Most of this is copied from miner/rustchain_amiga_miner.c (SHA-1, the
 * JSON scans, ipv4 parse, the bsdsocket request loop) and adapted from
 * POST to GET. See rtc_common.h for the m68k rules this file obeys.
 *
 * VENDORED COPY for claude/ (Phase 5, 2026-07-02).
 * Origin: rustchain-amiga/tools/common/rtc_common.c (verbatim) with ONE
 * adaptation for the Claude client: rtc_net_open() opens bsdsocket.library
 * v4 instead of v3, because AmiSSL's InitAmiSSL() wants the v4 API and the
 * Claude client shares this one SocketBase between its plain-HTTP proxy path
 * and the AmiSSL direct-HTTPS path. Nothing else changed.
 * The Claude client uses these helpers for JSON scans, HTTP header parsing,
 * SHA-1 and the bsdsocket break/sleep helpers; it provides its own POST and
 * TLS transports in claude.c.
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

#define ROM_BASE 0xF80000UL
#endif

#define ROM_SIZE 524288UL

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

void rtc_sha1_hex(const unsigned char *data, unsigned long n, char out[41])
{
    sha1_ctx c;
    unsigned char dig[20];
    int i;
    static const char hexd[] = "0123456789abcdef";

    sha1_init(&c);
    sha1_update(&c, data, n);
    sha1_final(&c, dig);
    for (i = 0; i < 20; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0x0F];
    }
    out[40] = '\0';
}

/* ------------------------------------------------------------------ */
/* JSON scans                                                         */
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

const char *rtc_json_array(const char *json, const char *key)
{
    char pat[64];
    const char *p;

    if ((int)strlen(key) > 60)
        return NULL;
    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return NULL;
    p = strchr(p + strlen(pat), '[');
    return p ? p + 1 : NULL;
}

const char *rtc_json_next_obj(const char *p, char *out, int outlen)
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

/* ------------------------------------------------------------------ */
/* HTTP                                                               */
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
        "User-Agent: rtctools-amiga/" RTC_TOOLS_VERSION "\r\n"
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
/* formatting                                                         */
/* ------------------------------------------------------------------ */

void rtc_age_str(unsigned long now, unsigned long then, char *out)
{
    unsigned long d;

    if (then == 0) {
        strcpy(out, "?");
        return;
    }
    d = (then >= now) ? 0 : now - then;
    if (d < 60)
        sprintf(out, "%lus", d);
    else if (d < 3600)
        sprintf(out, "%lum", d / 60);
    else if (d < 86400)
        sprintf(out, "%luh", d / 3600);
    else
        sprintf(out, "%lud", d / 86400);
}

/* ------------------------------------------------------------------ */
/* machine id                                                         */
/* ------------------------------------------------------------------ */

#ifndef HOST_TEST

static const char *detect_arch(void)
{
    unsigned short af = SysBase->AttnFlags;

    if (af & AFF_68060) return "68060";
    if (af & AFF_68040) return "68040";
    if (af & AFF_68030) return "68030";
    if (af & AFF_68020) return "68020";
    if (af & AFF_68010) return "68010";
    return "68000";
}

void rtc_machine_id(char *out, int outlen)
{
    char hash[41];
    char id[32];

    rtc_sha1_hex((const unsigned char *)ROM_BASE, ROM_SIZE, hash);
    sprintf(id, "amiga-%.6s-%.8s", detect_arch(), hash);
    strncpy(out, id, outlen - 1);
    out[outlen - 1] = '\0';
}

#else /* HOST_TEST */

/* same synthetic 512KB ROM pattern the miner's host stub uses, so the
   expected id is locked: sha1 = aabd0895... -> amiga-68030-aabd0895 */
void rtc_machine_id(char *out, int outlen)
{
    static unsigned char host_rom[ROM_SIZE];
    char hash[41];
    char id[32];
    unsigned long i;

    for (i = 0; i < ROM_SIZE; i++)
        host_rom[i] = (unsigned char)((i * 7 + 13) & 0xFF);
    rtc_sha1_hex(host_rom, ROM_SIZE, hash);
    sprintf(id, "amiga-%.6s-%.8s", "68030", hash);
    strncpy(out, id, outlen - 1);
    out[outlen - 1] = '\0';
}

#endif

/* ------------------------------------------------------------------ */
/* platform: network and break handling                               */
/* ------------------------------------------------------------------ */

#ifndef HOST_TEST

int rtc_net_open(void)
{
    if (SocketBase)
        return 1;
    /* v3, not v4: on AROS/FS-UAE bsdsocket, opening v4 works for socket I/O
       but CloseLibrary() on the v4 base HANGS at process exit (and leaking it
       poisons the next same-shell invocation). v3 opens and closes cleanly --
       this is exactly what the proven amiport client uses. The AmiSSL direct
       path re-checks the version it needs in amissl_init(). */
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

long rtc_http_get(const char *host, int port, const char *path,
                  char *resp, long resplen)
{
    static char req[1024];
    struct sockaddr_in sa;
    unsigned long ip;
    long s;
    long got, total = 0;
    int reqlen;

    reqlen = rtc_format_get(req, sizeof(req), host, port, path);
    if (reqlen < 0) {
        fprintf(stderr, "[FAIL] request too large\n");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (unsigned short)port;   /* m68k is big endian, htons is identity */

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

    if (send(s, req, reqlen, 0) != reqlen) {
        fprintf(stderr, "[FAIL] send failed\n");
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

int rtc_check_break(void)
{
    return (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

int rtc_sleep_break(int seconds)
{
    int i;

    for (i = 0; i < seconds; i++) {
        if (rtc_check_break())
            return 1;
        Delay(50);
    }
    return rtc_check_break();
}

#else /* HOST_TEST stubs */

int rtc_net_open(void)
{
    return 1;
}

void rtc_net_close(void)
{
}

long rtc_http_get(const char *host, int port, const char *path,
                  char *resp, long resplen)
{
    (void)host; (void)port; (void)path;
    if (resplen > 0)
        resp[0] = '\0';
    return -1;
}

int rtc_check_break(void)
{
    return 0;
}

int rtc_sleep_break(int seconds)
{
    (void)seconds;
    return 0;
}

#endif
