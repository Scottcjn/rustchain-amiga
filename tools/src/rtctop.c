/*
 * rtctop - one-shot RustChain network status for classic AmigaOS (m68k)
 *
 *   rtctop                 show the miner table once
 *   rtctop -l              refresh every 60 seconds until Ctrl-C
 *   rtctop --node h:p      use another node
 *
 * Data: GET /epoch and GET /api/miners. The miners reply is a wrapper
 * object {"miners":[...],"pagination":{...}} (verified live 2026-07-02,
 * NOT a bare array); each entry carries miner, device_arch,
 * antiquity_multiplier, last_attest. The server returns newest-attest
 * first and caps the list at 100; both are kept as-is.
 *
 * "now" is derived from the server's slot number
 * (genesis + (slot+1)*600), so attestation ages are sane even when the
 * Amiga clock is unset. Resolution is the 10 minute slot, good enough
 * for an age column.
 *
 * Table fits 77 columns for a stock 640 pixel Topaz-8 AmigaShell.
 *
 * Exit codes: 0 ok, 5 usage, 10 network error, 15 bad server reply.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc_common.h"

#define EPOCH_MAX 4096
#define MINERS_MAX 32768
#define OBJ_MAX 512
#define LINE_MAX 96

struct topinfo {
    char epoch[16];
    char supply[24];
    char enrolled[16];
    unsigned long slot;
    unsigned long now;
};

static void parse_epoch(const char *body, struct topinfo *ti)
{
    char v[24];

    memset(ti, 0, sizeof(*ti));
    strcpy(ti->epoch, "?");
    strcpy(ti->supply, "?");
    strcpy(ti->enrolled, "?");
    if (rtc_json_raw(body, "epoch", v, sizeof(v)))
        sprintf(ti->epoch, "%.15s", v);
    if (rtc_json_raw(body, "total_supply_rtc", v, sizeof(v)))
        sprintf(ti->supply, "%.23s", v);
    if (rtc_json_raw(body, "enrolled_miners", v, sizeof(v)))
        sprintf(ti->enrolled, "%.15s", v);
    if (rtc_json_raw(body, "slot", v, sizeof(v)))
        ti->slot = strtoul(v, NULL, 10);
    ti->now = RTC_GENESIS_TS + (ti->slot + 1) * RTC_SLOT_SECONDS;
}

/* one table row from one miner object. line needs LINE_MAX bytes.
   Widths: 41 + 1 + 12 + 1 + 6 + 1 + 5 = 67 chars, under 77. */
static int render_row(const char *obj, unsigned long now, char *line)
{
    char miner[128], arch[32], mult[16], attest[24], age[16];
    unsigned long ts;

    if (!rtc_json_raw(obj, "miner", miner, sizeof(miner)))
        return 0;
    if (!rtc_json_raw(obj, "device_arch", arch, sizeof(arch)))
        strcpy(arch, "?");
    if (!rtc_json_raw(obj, "antiquity_multiplier", mult, sizeof(mult)))
        strcpy(mult, "?");
    ts = rtc_json_raw(obj, "last_attest", attest, sizeof(attest))
             ? strtoul(attest, NULL, 10) : 0;
    rtc_age_str(now, ts, age);

    if (strlen(miner) > 41) {
        miner[38] = '.'; miner[39] = '.'; miner[40] = '.';
        miner[41] = '\0';
    }
    arch[12] = '\0';
    mult[6] = '\0';

    sprintf(line, "%-41s %-12s %6s %5s", miner, arch, mult, age);
    return 1;
}

static void print_header(void)
{
    printf("%-41s %-12s %6s %5s\n", "MINER", "ARCH", "MULT", "SEEN");
    printf("----------------------------------------- ------------"
           " ------ -----\n");
}

/* fetch one endpoint into resp, return body or NULL with rc set */
static const char *fetch(const char *host, int port, const char *path,
                         char *resp, long resplen, int *rc)
{
    const char *body;
    int status;

    if (rtc_http_get(host, port, path, resp, resplen) <= 0) {
        *rc = 10;
        return NULL;
    }
    status = rtc_http_status(resp);
    body = rtc_http_body(resp);
    if (status != 200 || !body) {
        fprintf(stderr, "rtctop: node answered HTTP %d for %s\n", status, path);
        *rc = 15;
        return NULL;
    }
    return body;
}

#ifndef HOST_TEST

static int show_status(const char *host, int port)
{
    static char eresp[EPOCH_MAX];
    static char mresp[MINERS_MAX];
    char obj[OBJ_MAX];
    char line[LINE_MAX];
    struct topinfo ti;
    const char *body, *p;
    int rc = 0, shown = 0;

    body = fetch(host, port, "/epoch", eresp, sizeof(eresp), &rc);
    if (!body)
        return rc;
    parse_epoch(body, &ti);

    body = fetch(host, port, "/api/miners", mresp, sizeof(mresp), &rc);
    if (!body)
        return rc;

    printf("RustChain network status  (%s:%d)  epoch %s  slot %lu\n\n",
           host, port, ti.epoch, ti.slot);
    print_header();

    p = rtc_json_array(body, "miners");
    if (!p) {
        fprintf(stderr, "rtctop: no miners array in reply\n");
        return 15;
    }
    while ((p = rtc_json_next_obj(p, obj, sizeof(obj))) != NULL) {
        if (render_row(obj, ti.now, line)) {
            printf("%s\n", line);
            shown++;
        }
    }

    printf("\n%d shown, %s enrolled | epoch %s | supply %s RTC\n",
           shown, ti.enrolled, ti.epoch, ti.supply);
    return 0;
}

int main(int argc, char **argv)
{
    char host[128];
    int port = RTC_DEFAULT_PORT;
    int loop = 0;
    int i, rc = 0;

    strcpy(host, RTC_DEFAULT_HOST);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            loop = 1;
        } else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            char *colon;
            strncpy(host, argv[++i], sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            colon = strchr(host, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
        } else {
            printf("rtctop " RTC_TOOLS_VERSION " - RustChain network status\n");
            printf("usage: rtctop [-l] [--node host:port]\n");
            printf("  -l    refresh every 60s until Ctrl-C\n");
            return 5;
        }
    }

    if (!rtc_net_open())
        return 10;

    for (;;) {
        rc = show_status(host, port);
        if (!loop || rc == 10)
            break;
        printf("\n(refresh in 60s, Ctrl-C stops)\n");
        if (rtc_sleep_break(60)) {
            printf("^C\n");
            rc = 0;
            break;
        }
        printf("\n");
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
    static char miners[MINERS_MAX];
    static char epoch[EPOCH_MAX];
    char obj[OBJ_MAX];
    char line[LINE_MAX];
    char age[16];
    struct topinfo ti;
    const char *p;
    int rows = 0, widths_ok = 1, first = 1;

    printf("rtctop host test (real captured node responses)\n");

    printf("age formatting:\n");
    rtc_age_str(1000, 990, age);  check_str("10 seconds", age, "10s");
    rtc_age_str(1000, 0, age);    check_str("never seen", age, "?");
    rtc_age_str(1000, 2000, age); check_str("future clamps to 0s", age, "0s");
    rtc_age_str(4000, 500, age);  check_str("minutes", age, "58m");
    rtc_age_str(4000, 400, age);  check_str("3600s rolls to hours", age, "1h");
    rtc_age_str(90000, 10000, age); check_str("hours", age, "22h");
    rtc_age_str(900000, 100, age);check_str("days", age, "10d");

    printf("epoch parsing and slot-derived now (testdata/epoch.json):\n");
    if (slurp("testdata/epoch.json", epoch, sizeof(epoch)) > 0) {
        parse_epoch(epoch, &ti);
        check_str("epoch", ti.epoch, "211");
        check_str("supply", ti.supply, "8388608");
        check_str("enrolled", ti.enrolled, "23");
        check("slot 30487", ti.slot == 30487UL);
        /* 1764706927 + 30488*600 = 1782999727 */
        check("now derived from slot", ti.now == 1782999727UL);
    }

    printf("miner table from the real /api/miners capture:\n");
    if (slurp("testdata/miners.json", miners, sizeof(miners)) > 0) {
        p = rtc_json_array(miners, "miners");
        check("miners array found (wrapper object handled)", p != NULL);
        print_header();
        while ((p = rtc_json_next_obj(p, obj, sizeof(obj))) != NULL) {
            if (!render_row(obj, ti.now, line))
                continue;
            if (strlen(line) > 77)
                widths_ok = 0;
            if (first) {
                printf("        | %s\n", line);
                check("first row is the M3 miner",
                      strstr(line, "M3") != NULL &&
                      strstr(line, "1.1") != NULL);
                check("43-char RTC id truncated to 38+... = 41",
                      strstr(line, "RTC41e11e938fc3cb4f77060cca50b89289599...")
                      != NULL);
                first = 0;
            }
            rows++;
        }
        check("20 rows rendered (pagination count)", rows == 20);
        check("every line fits 77 columns", widths_ok);
    }

    printf("row rendering corner cases:\n");
    check("object without miner key skipped",
          !render_row("{\"device_arch\":\"G4\"}", 1000, line));
    check("missing fields become ?",
          render_row("{\"miner\":\"x\"}", 1000, line) &&
          strstr(line, "?") != NULL);
    {
        const char *g4 = "{\"miner\":\"dual-g4-125\",\"device_arch\":\"G4\","
                         "\"antiquity_multiplier\":2.5,"
                         "\"last_attest\":1782999027}";
        check("short id kept whole, mult raw",
              render_row(g4, 1782999727UL, line) &&
              strncmp(line, "dual-g4-125 ", 12) == 0 &&
              strstr(line, "2.5") != NULL &&
              strstr(line, "11m") != NULL);
    }

    printf("clean failure without network (host stub):\n");
    {
        static char resp[EPOCH_MAX];
        int rc = 0;
        check("fetch fails with rc 10",
              fetch("127.0.0.1", 1, "/epoch", resp, sizeof(resp), &rc) == NULL
              && rc == 10);
    }

    printf("\n%s (%d failures)\n",
           failures ? "HOST TEST FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 20 : 0;
}

#endif /* HOST_TEST */
