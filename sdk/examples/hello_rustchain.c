/*
 * hello_rustchain.c - fetch the node's /health endpoint and print it.
 * Smallest possible librustchain program.
 *
 * Build (see docs/QUICKSTART.md):
 *   m68k-amigaos-gcc -noixemul -m68000 -O2 -Iinclude \
 *     -o hello_rustchain examples/hello_rustchain.c lib/librustchain.a
 *
 * Usage: hello_rustchain [--node host:port]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rustchain/rc_http.h>

int main(int argc, char **argv)
{
    static char resp[RC_RESP_MAX];
    char host[128];
    int port = 8088, n, i;
    const char *body;

    strcpy(host, "50.28.86.131");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            char *colon;
            strncpy(host, argv[++i], sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            colon = strchr(host, ':');
            if (colon) { *colon = '\0'; port = atoi(colon + 1); }
        } else {
            printf("usage: hello_rustchain [--node host:port]\n");
            return 5;
        }
    }

    if (!rc_http_open())
        return 10;

    printf("GET http://%s:%d/health\n", host, port);
    n = rc_http_get(host, port, "/health", resp, sizeof(resp));
    rc_http_close();
    if (n <= 0) {
        printf("[FAIL] no response\n");
        return 10;
    }

    printf("HTTP %d\n", rc_http_status(resp));
    body = rc_http_body(resp);
    if (body)
        printf("%s\n", body);
    return 0;
}
