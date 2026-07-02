/*
 * rc_http.c - plain HTTP/1.1 over bsdsocket.library.
 * Extracted from the working miner (rustchain_amiga_miner.c), behavior
 * identical for POST; GET added the same way for tools like rtcfetch.
 * The formatters and parsers are portable; the socket path is
 * Amiga-only and becomes failing stubs under -DHOST_TEST.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rustchain/rc_http.h"

#ifndef HOST_TEST
#include <exec/types.h>
#include <proto/exec.h>

struct Library *SocketBase = NULL;

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>
#endif

/* ------------------------------------------------------------------ */
/* portable formatting and parsing                                    */
/* ------------------------------------------------------------------ */

/*
 * The fixed header text in the POST/GET templates is under 128 bytes;
 * port and Content-Length add at most 11 digits each. 160 covers both
 * with room to spare, so the pre-checks below are safe upper bounds.
 * Fail closed: if the request might not fit, refuse to build it, never
 * hand a truncated request to the socket.
 */
#define RC_HTTP_OVERHEAD (160 + (int)sizeof(RC_HTTP_UA))

int rc_http_format_post(char *out, int outlen,
                        const char *host, int port,
                        const char *path, const char *body)
{
    int len = (int)strlen(body);
    int n;

    if ((int)strlen(path) + (int)strlen(host) + len + RC_HTTP_OVERHEAD >= outlen)
        return -1;

    n = sprintf(out,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: " RC_HTTP_UA "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, len, body);

    if (n >= outlen) return -1;
    return n;
}

int rc_http_format_get(char *out, int outlen,
                       const char *host, int port, const char *path)
{
    int n;

    if ((int)strlen(path) + (int)strlen(host) + RC_HTTP_OVERHEAD >= outlen)
        return -1;

    n = sprintf(out,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: " RC_HTTP_UA "\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, port);

    if (n >= outlen) return -1;
    return n;
}

int rc_http_status(const char *resp)
{
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    return atoi(resp + 9);
}

const char *rc_http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* ------------------------------------------------------------------ */
/* Amiga transport                                                    */
/* ------------------------------------------------------------------ */

#ifndef HOST_TEST

static int socket_opened_here = 0;

int rc_http_open(void)
{
    if (SocketBase) return 1;
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        printf("[FAIL] bsdsocket.library not available\n");
        printf("       start a TCP/IP stack (Roadshow/AmiTCP) or enable\n");
        printf("       bsdsocket_library = 1 in FS-UAE\n");
        return 0;
    }
    socket_opened_here = 1;
    return 1;
}

void rc_http_close(void)
{
    if (SocketBase && socket_opened_here) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
        socket_opened_here = 0;
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

/* send a preformatted request, read the whole response */
static int do_request(const char *host, int port,
                      const char *req, int reqlen,
                      char *resp, int resplen)
{
    struct sockaddr_in sa;
    unsigned long ip;
    long s;
    int got, total = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (unsigned short)port;   /* m68k is big endian, htons is identity */

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            printf("[FAIL] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("[FAIL] socket() failed\n");
        return -1;
    }

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("[FAIL] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }

    if (send(s, (char *)req, reqlen, 0) != reqlen) {
        printf("[FAIL] send failed\n");
        CloseSocket(s);
        return -1;
    }

    while (total < resplen - 1) {
        got = recv(s, resp + total, resplen - 1 - total, 0);
        if (got <= 0) break;
        total += got;
    }
    resp[total] = '\0';
    CloseSocket(s);

    return total;
}

int rc_http_post(const char *host, int port,
                 const char *path, const char *body,
                 char *resp, int resplen)
{
    static char req[RC_HTTP_MAX];
    int reqlen;

    reqlen = rc_http_format_post(req, sizeof(req), host, port, path, body);
    if (reqlen < 0) {
        printf("[FAIL] request too large\n");
        return -1;
    }
    return do_request(host, port, req, reqlen, resp, resplen);
}

int rc_http_get(const char *host, int port, const char *path,
                char *resp, int resplen)
{
    static char req[RC_HTTP_MAX];
    int reqlen;

    reqlen = rc_http_format_get(req, sizeof(req), host, port, path);
    if (reqlen < 0) {
        printf("[FAIL] request too large\n");
        return -1;
    }
    return do_request(host, port, req, reqlen, resp, resplen);
}

#else /* HOST_TEST: no network, stubs fail cleanly */

int rc_http_open(void)
{
    return 0;
}

void rc_http_close(void)
{
}

int rc_http_post(const char *host, int port,
                 const char *path, const char *body,
                 char *resp, int resplen)
{
    (void)host; (void)port; (void)path; (void)body;
    if (resplen > 0) resp[0] = '\0';
    return -1;
}

int rc_http_get(const char *host, int port, const char *path,
                char *resp, int resplen)
{
    (void)host; (void)port; (void)path;
    if (resplen > 0) resp[0] = '\0';
    return -1;
}

#endif /* HOST_TEST */
