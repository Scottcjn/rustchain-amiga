/*
 * attest_demo.c - minimal RustChain attestation using librustchain.
 *
 * The point of this file: the whole challenge/submit dance, hardware
 * detection included, is about a dozen lines of SDK calls.
 *
 * Build (see docs/QUICKSTART.md):
 *   m68k-amigaos-gcc -noixemul -m68000 -O2 -Iinclude \
 *     -o attest_demo examples/attest_demo.c lib/librustchain.a
 *
 * Usage: attest_demo [--node host:port] [--wallet id] [--dry-run]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rustchain/rc_attest.h>
#include <rustchain/rc_http.h>

int main(int argc, char **argv)
{
    struct rc_hwinfo hw;
    char host[128];
    char wallet[96];
    int port = RC_DEFAULT_NODE_PORT, dry_run = 0, ok = 0, i;

    strcpy(host, RC_DEFAULT_NODE_HOST);
    strcpy(wallet, "amiga-sdk-demo");

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            char *colon;
            strncpy(host, argv[++i], sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            colon = strchr(host, ':');
            if (colon) { *colon = '\0'; port = atoi(colon + 1); }
        }
        else if (strcmp(argv[i], "--wallet") == 0 && i + 1 < argc) {
            strncpy(wallet, argv[++i], sizeof(wallet) - 1);
            wallet[sizeof(wallet) - 1] = '\0';
        }
        else {
            printf("usage: attest_demo [--node host:port] [--wallet id] [--dry-run]\n");
            return 5;
        }
    }

    printf("librustchain attest demo\n");
    rc_hw_detect(&hw);
    rc_hw_print(&hw);

    if (dry_run) {
        static char payload[RC_PAYLOAD_MAX];
        struct rc_entropy ent;
        struct rc_emu emu;
        rc_entropy_collect(&ent);
        rc_emu_detect(&emu);
        if (rc_attest_build_payload(payload, sizeof(payload), &hw, wallet,
                                    wallet, "dry-run-nonce", &ent, &emu) > 0)
            printf("--- payload for POST http://%s:%d/attest/submit ---\n%s\n",
                   host, port, payload);
        rc_timer_cleanup();
        return 0;
    }

    if (!rc_http_open())
        return 10;
    ok = rc_attest_once(host, port, wallet, wallet, &hw);
    rc_http_close();
    rc_timer_cleanup();
    return ok ? 0 : 10;
}
