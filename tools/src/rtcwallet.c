/*
 * rtcwallet - read-only RustChain wallet client for classic AmigaOS (m68k)
 *
 *   rtcwallet balance <miner_id>   show RTC balance
 *   rtcwallet epoch                show current epoch info
 *   rtcwallet address              print this machine's miner id convention
 *   rtcwallet --node host:port ... use another node
 *
 * READ-ONLY on purpose: transfers need Ed25519 signatures
 * (/wallet/transfer/signed) and there is no Ed25519 here yet. Use the
 * desktop wallet for sending.
 *
 * Endpoints (probed live on 50.28.86.131:8088, 2026-07-02):
 *   GET /wallet/balance?miner_id=X -> {"amount_i64":..,"amount_rtc":..,"miner_id":".."}
 *   GET /epoch -> {"blocks_per_epoch":144,"enrolled_miners":..,"epoch":..,
 *                  "epoch_pot":..,"slot":..,"total_supply_rtc":..}
 *   Plain /balance and /api/balance are 404, do not use them.
 *   Unknown miners return 200 with amount_rtc 0.0, not an error.
 *
 * Exit codes: 0 ok, 5 usage, 10 network error, 15 bad server reply.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_common.h"

#define RESP_MAX 8192

#ifndef HOST_TEST
static void usage(void)
{
    printf("rtcwallet " RTC_TOOLS_VERSION " - read-only RustChain wallet client\n");
    printf("usage: rtcwallet [--node host:port] <command>\n");
    printf("  balance <miner_id>   show RTC balance for a miner id\n");
    printf("  epoch                show current epoch / network info\n");
    printf("  address              print this machine's miner id convention\n");
    printf("default node: " RTC_DEFAULT_HOST ":%d\n", RTC_DEFAULT_PORT);
    printf("read-only: transfers need Ed25519 signing, not available here\n");
}
#endif

/* %XX-escape anything that is not URL-safe so odd miner ids cannot
   break the query string */
static void query_escape(char *dst, int dstlen, const char *src)
{
    static const char hexd[] = "0123456789ABCDEF";
    int o = 0;

    while (*src && o < dstlen - 4) {
        unsigned char ch = (unsigned char)*src++;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
            dst[o++] = (char)ch;
        } else {
            dst[o++] = '%';
            dst[o++] = hexd[ch >> 4];
            dst[o++] = hexd[ch & 0x0F];
        }
    }
    dst[o] = '\0';
}

/* fetch path from the node, verify HTTP 200, return body pointer into
   resp or NULL (message already printed). rc gets the exit code. */
static const char *fetch(const char *host, int port, const char *path,
                         char *resp, long resplen, int *rc)
{
    const char *body;
    int status;

    if (!rtc_net_open()) {
        *rc = 10;
        return NULL;
    }
    if (rtc_http_get(host, port, path, resp, resplen) <= 0) {
        *rc = 10;
        return NULL;
    }
    status = rtc_http_status(resp);
    body = rtc_http_body(resp);
    if (status != 200 || !body) {
        fprintf(stderr, "rtcwallet: node answered HTTP %d for %s\n",
                status, path);
        *rc = 15;
        return NULL;
    }
    return body;
}

static int cmd_balance(const char *host, int port, const char *miner_id)
{
    static char resp[RESP_MAX];
    char path[320];
    char enc[224];
    char amount[32], id[128];
    const char *body;
    int rc = 0;

    query_escape(enc, sizeof(enc), miner_id);
    sprintf(path, "/wallet/balance?miner_id=%s", enc);

    body = fetch(host, port, path, resp, sizeof(resp), &rc);
    if (!body)
        return rc;

    if (!rtc_json_raw(body, "amount_rtc", amount, sizeof(amount))) {
        fprintf(stderr, "rtcwallet: no amount_rtc in reply\n");
        return 15;
    }
    if (!rtc_json_raw(body, "miner_id", id, sizeof(id)))
        strcpy(id, miner_id);

    printf("miner:   %s\n", id);
    printf("balance: %s RTC\n", amount);
    return 0;
}

static int cmd_epoch(const char *host, int port)
{
    static char resp[RESP_MAX];
    char v[32];
    const char *body;
    int rc = 0;

    body = fetch(host, port, "/epoch", resp, sizeof(resp), &rc);
    if (!body)
        return rc;

    printf("RustChain epoch info (%s:%d)\n", host, port);
    if (rtc_json_raw(body, "epoch", v, sizeof(v)))
        printf("  epoch:            %s\n", v);
    if (rtc_json_raw(body, "slot", v, sizeof(v)))
        printf("  slot:             %s\n", v);
    if (rtc_json_raw(body, "blocks_per_epoch", v, sizeof(v)))
        printf("  blocks per epoch: %s\n", v);
    if (rtc_json_raw(body, "enrolled_miners", v, sizeof(v)))
        printf("  enrolled miners:  %s\n", v);
    if (rtc_json_raw(body, "epoch_pot", v, sizeof(v)))
        printf("  epoch pot:        %s RTC\n", v);
    if (rtc_json_raw(body, "total_supply_rtc", v, sizeof(v)))
        printf("  total supply:     %s RTC\n", v);
    return 0;
}

static int cmd_address(void)
{
    char id[32];

    rtc_machine_id(id, sizeof(id));
    printf("machine miner id: %s\n", id);
    printf("  convention: amiga-<cpu>-<first 8 hex of ROM SHA-1>\n");
    printf("  a suggested per-machine id, not a funded wallet; the miner\n");
    printf("  uses whatever --wallet you give it\n");
    return 0;
}

#ifndef HOST_TEST

int main(int argc, char **argv)
{
    char host[128];
    int port = RTC_DEFAULT_PORT;
    int i = 1;
    int rc;

    strcpy(host, RTC_DEFAULT_HOST);

    if (i < argc && strcmp(argv[i], "--node") == 0) {
        char *colon;
        if (i + 1 >= argc) {
            usage();
            return 5;
        }
        strncpy(host, argv[i + 1], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
        colon = strchr(host, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        i += 2;
    }

    if (i >= argc) {
        usage();
        return 5;
    }

    if (strcmp(argv[i], "balance") == 0) {
        if (i + 1 >= argc) {
            fprintf(stderr, "rtcwallet: balance needs a miner id\n");
            usage();
            return 5;
        }
        rc = cmd_balance(host, port, argv[i + 1]);
    } else if (strcmp(argv[i], "epoch") == 0) {
        rc = cmd_epoch(host, port);
    } else if (strcmp(argv[i], "address") == 0) {
        rc = cmd_address();
    } else {
        usage();
        return 5;
    }

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

static long slurp(const char *fn, char *buf, long buflen)
{
    FILE *f = fopen(fn, "rb");
    long n;

    if (!f) {
        printf("  [FAIL] cannot open %s (run from tools/)\n", fn);
        failures++;
        return -1;
    }
    n = (long)fread(buf, 1, buflen - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

int main(void)
{
    static char buf[16384];
    char v[128];
    char hex[41];

    printf("rtcwallet host test (real captured node responses)\n");

    printf("SHA-1 sanity (NIST vector, common code):\n");
    rtc_sha1_hex((const unsigned char *)"abc", 3, hex);
    check_str("sha1(abc)", hex, "a9993e364706816aba3e25717850c26c9cd0d89d");

    printf("balance parsing (testdata/balance.json):\n");
    if (slurp("testdata/balance.json", buf, sizeof(buf)) > 0) {
        check("amount_rtc found", rtc_json_raw(buf, "amount_rtc", v, sizeof(v)));
        check_str("amount_rtc value", v, "718.514429");
        check("miner_id found", rtc_json_raw(buf, "miner_id", v, sizeof(v)));
        check_str("miner_id value", v, "dual-g4-125");
        check("amount_i64 raw token", rtc_json_raw(buf, "amount_i64", v, sizeof(v))
              && strcmp(v, "718514429") == 0);
    }

    printf("epoch parsing (testdata/epoch.json):\n");
    if (slurp("testdata/epoch.json", buf, sizeof(buf)) > 0) {
        check("epoch found", rtc_json_raw(buf, "epoch", v, sizeof(v)));
        check_str("epoch value", v, "211");
        check("total_supply_rtc", rtc_json_raw(buf, "total_supply_rtc", v, sizeof(v))
              && strcmp(v, "8388608") == 0);
        check("epoch_pot is 1.5 raw", rtc_json_raw(buf, "epoch_pot", v, sizeof(v))
              && strcmp(v, "1.5") == 0);
        check("missing key returns 0", !rtc_json_raw(buf, "no_such_key", v, sizeof(v)));
    }

    printf("raw HTTP response (testdata/epoch_raw.http):\n");
    if (slurp("testdata/epoch_raw.http", buf, sizeof(buf)) > 0) {
        const char *body = rtc_http_body(buf);
        check("status 200", rtc_http_status(buf) == 200);
        check("body found", body != NULL);
        if (body) {
            check("body parses", rtc_json_raw(body, "blocks_per_epoch", v, sizeof(v))
                  && strcmp(v, "144") == 0);
        }
    }

    printf("query escaping:\n");
    {
        char enc[224];
        query_escape(enc, sizeof(enc), "dual-g4-125");
        check_str("safe id unchanged", enc, "dual-g4-125");
        query_escape(enc, sizeof(enc), "bad id&x=1");
        check_str("unsafe chars escaped", enc, "bad%20id%26x%3D1");
    }

    printf("machine id convention:\n");
    {
        char id[32];
        rtc_machine_id(id, sizeof(id));
        check_str("stub machine id", id, "amiga-68030-aabd0895");
    }

    printf("commands and clean failure paths:\n");
    check("cmd_address exit 0", cmd_address() == 0);
    {
        /* the host stub rtc_http_get returns -1, so every network
           command must come back with exit code 10, no crash */
        static char resp[RESP_MAX];
        int rc = 0;
        const char *b = fetch("127.0.0.1", 1, "/epoch", resp, sizeof(resp), &rc);
        check("fetch fails cleanly without network", b == NULL && rc == 10);
        check("cmd_balance network fail exit 10",
              cmd_balance("127.0.0.1", 1, "x") == 10);
        check("cmd_epoch network fail exit 10",
              cmd_epoch("127.0.0.1", 1) == 10);
    }

    printf("\n%s (%d failures)\n",
           failures ? "HOST TEST FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 20 : 0;
}

#endif /* HOST_TEST */
