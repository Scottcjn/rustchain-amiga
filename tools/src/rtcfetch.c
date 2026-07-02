/*
 * rtcfetch - minimal HTTP fetch for classic AmigaOS (m68k), the Amiga's
 * curl-lite for RustChain endpoints.
 *
 *   rtcfetch <url>                 print body to stdout
 *   rtcfetch <url> <outfile>       save body to a file
 *   rtcfetch -m <kb> <url> ...     raise the size cap (default 64 KB,
 *                                  max 1024 KB)
 *
 * http:// only. https needs TLS which no stock Amiga stack provides;
 * point it at the node's plain-HTTP port (50.28.86.131:8088) or any
 * plain HTTP server. Requests go out as HTTP/1.0 so the reply is never
 * chunked. Redirects are not followed; the Location header is printed
 * to stderr so you can rerun by hand.
 *
 * Exit codes: 0 ok (2xx), 5 usage, 10 network error, 15 HTTP error status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_common.h"

#define DEFAULT_CAP_KB 64
#define MAX_CAP_KB 1024
#define HEADER_ROOM 16384

#ifndef HOST_TEST
static void usage(void)
{
    printf("rtcfetch " RTC_TOOLS_VERSION " - minimal HTTP fetch (http:// only)\n");
    printf("usage: rtcfetch [-m maxkb] <url> [outfile]\n");
    printf("  -m <kb>   body size cap in KB (default %d, max %d)\n",
           DEFAULT_CAP_KB, MAX_CAP_KB);
    printf("example: rtcfetch http://" RTC_DEFAULT_HOST ":8088/epoch\n");
}
#endif

/* find the Location header for redirect hints; out untouched if absent */
static int find_location(const char *resp, char *out, int outlen)
{
    const char *p = strstr(resp, "\r\nLocation:");
    int o = 0;

    if (!p)
        p = strstr(resp, "\r\nlocation:");
    if (!p)
        return 0;
    p += 11;
    while (*p == ' ')
        p++;
    while (*p && *p != '\r' && *p != '\n' && o < outlen - 1)
        out[o++] = *p++;
    out[o] = '\0';
    return o > 0;
}

/* fetch url into a malloc'd buffer, write body to out (stdout or file).
   cap_kb caps the total response size. */
static int do_fetch(const char *url, const char *outfile, int cap_kb)
{
    char host[128];
    char path[512];
    char loc[256];
    int port = 0;
    char *buf;
    long buflen, got, bodylen;
    const char *body;
    int status;
    FILE *out = stdout;

    if (!rtc_parse_url(url, host, sizeof(host), &port, path, sizeof(path))) {
        if (strncmp(url, "https://", 8) == 0)
            fprintf(stderr, "rtcfetch: https not supported, http:// only\n");
        else
            fprintf(stderr, "rtcfetch: cannot parse url: %s\n", url);
        return 5;
    }

    buflen = (long)cap_kb * 1024 + HEADER_ROOM;
    buf = (char *)malloc((size_t)buflen);
    if (!buf) {
        fprintf(stderr, "rtcfetch: no memory for %d KB buffer\n", cap_kb);
        return 10;
    }

    if (!rtc_net_open()) {
        free(buf);
        return 10;
    }
    got = rtc_http_get(host, port, path, buf, buflen);
    if (got <= 0) {
        free(buf);
        return 10;
    }

    status = rtc_http_status(buf);
    body = rtc_http_body(buf);
    if (status < 0 || !body) {
        fprintf(stderr, "rtcfetch: reply is not HTTP\n");
        free(buf);
        return 15;
    }
    bodylen = got - (long)(body - buf);

    if (got >= buflen - 1)
        fprintf(stderr, "rtcfetch: warning: hit the %d KB cap, body truncated"
                " (raise with -m)\n", cap_kb);
    if (status >= 300 && status < 400 && find_location(buf, loc, sizeof(loc)))
        fprintf(stderr, "rtcfetch: HTTP %d redirect to %s (not followed)\n",
                status, loc);

    if (outfile) {
        out = fopen(outfile, "wb");
        if (!out) {
            fprintf(stderr, "rtcfetch: cannot write %s\n", outfile);
            free(buf);
            return 10;
        }
    }
    if (bodylen > 0)
        fwrite(body, 1, (size_t)bodylen, out);
    if (outfile) {
        fclose(out);
        fprintf(stderr, "rtcfetch: HTTP %d, %ld bytes -> %s\n",
                status, bodylen, outfile);
    } else {
        fprintf(stderr, "rtcfetch: HTTP %d, %ld bytes\n", status, bodylen);
    }

    free(buf);
    return (status >= 200 && status < 300) ? 0 : 15;
}

#ifndef HOST_TEST

int main(int argc, char **argv)
{
    const char *url = NULL, *outfile = NULL;
    int cap_kb = DEFAULT_CAP_KB;
    int i;
    int rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            cap_kb = atoi(argv[++i]);
            if (cap_kb < 1 || cap_kb > MAX_CAP_KB) {
                fprintf(stderr, "rtcfetch: -m must be 1..%d\n", MAX_CAP_KB);
                return 5;
            }
        } else if (!url) {
            url = argv[i];
        } else if (!outfile) {
            outfile = argv[i];
        } else {
            usage();
            return 5;
        }
    }

    if (!url) {
        usage();
        return 5;
    }

    rc = do_fetch(url, outfile, cap_kb);
    rtc_net_close();
    return rc;
}

#else /* HOST_TEST */

static int failures = 0;

static void check(const char *name, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) failures++;
}

static void check_str(const char *name, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        printf("        got:  %s\n        want: %s\n", got, want);
        failures++;
    }
}

int main(void)
{
    char host[128], path[512];
    int port;

    printf("rtcfetch host test\n");

    printf("url parsing:\n");
    check("plain host", rtc_parse_url("http://example.com", host, sizeof(host),
          &port, path, sizeof(path)) && strcmp(host, "example.com") == 0 &&
          port == 80 && strcmp(path, "/") == 0);
    check("host with port and path",
          rtc_parse_url("http://50.28.86.131:8088/epoch", host, sizeof(host),
          &port, path, sizeof(path)) && strcmp(host, "50.28.86.131") == 0 &&
          port == 8088 && strcmp(path, "/epoch") == 0);
    check("query string kept",
          rtc_parse_url("http://n:8088/wallet/balance?miner_id=g4", host,
          sizeof(host), &port, path, sizeof(path)) &&
          strcmp(path, "/wallet/balance?miner_id=g4") == 0);
    check("colon in path is not a port",
          rtc_parse_url("http://h/a:b", host, sizeof(host), &port, path,
          sizeof(path)) && port == 80 && strcmp(path, "/a:b") == 0);
    check("https rejected", !rtc_parse_url("https://x.com", host, sizeof(host),
          &port, path, sizeof(path)));
    check("ftp rejected", !rtc_parse_url("ftp://x.com", host, sizeof(host),
          &port, path, sizeof(path)));
    check("empty host rejected", !rtc_parse_url("http:///x", host,
          sizeof(host), &port, path, sizeof(path)));
    check("bad port rejected", !rtc_parse_url("http://h:99999/", host,
          sizeof(host), &port, path, sizeof(path)));

    printf("request formatting (HTTP/1.0, no chunked replies):\n");
    {
        char req[1024];
        int n = rtc_format_get(req, sizeof(req), "50.28.86.131", 8088, "/epoch");
        check("request builds", n > 0);
        check("GET line", strncmp(req, "GET /epoch HTTP/1.0\r\n", 21) == 0);
        check("host header", strstr(req, "Host: 50.28.86.131:8088\r\n") != NULL);
        check("connection close", strstr(req, "Connection: close\r\n") != NULL);
        check("ends with blank line", strcmp(req + n - 4, "\r\n\r\n") == 0);
    }

    printf("response handling (real capture, testdata/epoch_raw.http):\n");
    {
        static char raw[4096];
        FILE *f = fopen("testdata/epoch_raw.http", "rb");
        long n = 0;
        if (f) {
            n = (long)fread(raw, 1, sizeof(raw) - 1, f);
            fclose(f);
        }
        raw[n] = '\0';
        check("capture loaded (run from tools/)", n > 0);
        check("status 200", rtc_http_status(raw) == 200);
        check("body is the epoch json", rtc_http_body(raw) != NULL &&
              strncmp(rtc_http_body(raw), "{\"blocks_per_epoch\"", 19) == 0);
    }

    printf("redirect header scan:\n");
    {
        char loc[256];
        const char *r = "HTTP/1.1 302 Found\r\nServer: x\r\n"
                        "Location: http://elsewhere/x\r\n\r\nmoved";
        check("location found", find_location(r, loc, sizeof(loc)));
        check_str("location value", loc, "http://elsewhere/x");
        check("absent location", !find_location("HTTP/1.1 200 OK\r\n\r\n",
              loc, sizeof(loc)));
    }

    printf("clean failure without network (host stub):\n");
    check("do_fetch network fail exit 10",
          do_fetch("http://127.0.0.1:1/x", NULL, 4) == 10);
    check("do_fetch bad url exit 5",
          do_fetch("gopher://old", NULL, 4) == 5);

    printf("\n%s (%d failures)\n",
           failures ? "HOST TEST FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 20 : 0;
}

#endif /* HOST_TEST */
