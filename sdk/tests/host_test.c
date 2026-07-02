/*
 * host_test.c - librustchain host smoke test.
 *
 * Compiled with -DHOST_TEST against the SDK sources, so the Amiga
 * calls are stubbed with the same fixed values the miner's host test
 * used. Every regression vector from the shipped miner carries over:
 * SHA-1 NIST vectors, the synthetic 512KB ROM, the 64-bit variance
 * overflow numbers, the shift-free isqrt64 table (2^34 bug), the u64
 * printer, the composed pipeline at live-run magnitudes, payload field
 * checks and the fingerprint block in both variants.
 *
 * Run "host_test --payload" to print only the JSON payload, so the
 * Makefile can pipe it through python3 json.loads.
 */

#include <stdio.h>
#include <string.h>

#include "rustchain/rc_attest.h"
#include "rustchain/rc_http.h"
#include "rustchain/rc_hw.h"
#include "rustchain/rc_json.h"
#include "rustchain/rc_sha1.h"
#include "rustchain/rc_u64.h"

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

/* build the reference payload the same way the miner's host test did */
static int build_ref_payload(char *payload, int cap)
{
    struct rc_hwinfo hw;
    struct rc_entropy ent;
    struct rc_emu emu;

    rc_hw_detect(&hw);
    rc_entropy_collect(&ent);
    rc_emu_detect(&emu);
    return rc_attest_build_payload(payload, cap, &hw,
                                   "amiga-fsuae-scott", "amiga-fsuae-scott",
                                   "cafebabe1234", &ent, &emu);
}

int main(int argc, char **argv)
{
    char hex[41];
    struct rc_hwinfo hw;
    struct rc_entropy ent;
    struct rc_emu emu;
    static char payload[RC_PAYLOAD_MAX];
    static char req[RC_HTTP_MAX];
    char nonce_out[160];
    int plen, rlen;

    if (argc > 1 && strcmp(argv[1], "--payload") == 0) {
        plen = build_ref_payload(payload, sizeof(payload));
        if (plen <= 0) return 20;
        printf("%s\n", payload);
        return 0;
    }

    printf("librustchain host smoke test\n");

    printf("SHA-1 vectors:\n");
    rc_sha1_hex((const unsigned char *)"", 0, hex);
    check_str("sha1(empty)", hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    rc_sha1_hex((const unsigned char *)"abc", 3, hex);
    check_str("sha1(abc)", hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
    rc_sha1_hex((const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, hex);
    check_str("sha1(fox)", hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    printf("ROM hash (512KB synthetic pattern):\n");
    rc_hw_detect(&hw);
    rc_hw_print(&hw);
    rc_sleep(0);
    rc_timer_cleanup();
    check_str("sha1(rom pattern)", hw.rom_hash,
              "aabd0895b5c2ec38884f5d54cff3fc6aab4fe7ba");
    check("rom_size is 524288", hw.rom_size == 524288UL);
    check("rom_checksum byte-sum", hw.rom_checksum == 66846720UL);
    check_str("arch stub", hw.arch, "68030");
    check_str("kick stub", hw.kick_version, "47.96");

    printf("64-bit entropy math (regression for the variance overflow bug):\n");
    rc_entropy_collect(&ent);
    {
        char b[24];
        check_str("rc_u64s(6712609375)", rc_u64s(6712609375ULL, b), "6712609375");
        check_str("rc_u64s(0)", rc_u64s(0ULL, b), "0");
        check_str("rc_u64s(17179869184)", rc_u64s(17179869184ULL, b), "17179869184");
        check_str("rc_u64s(28133137871)", rc_u64s(28133137871ULL, b), "28133137871");
        check_str("rc_u64s(u64 max)", rc_u64s(18446744073709551615ULL, b),
                  "18446744073709551615");
    }

    printf("isqrt64 (shift-free, regression for the m68k 2^34 stdev bug):\n");
    {
        static const struct { unsigned long long x, r; } sq[] = {
            {0ULL, 0ULL}, {1ULL, 1ULL}, {2ULL, 1ULL}, {3ULL, 1ULL},
            {4ULL, 2ULL}, {15ULL, 3ULL}, {16ULL, 4ULL}, {17ULL, 4ULL},
            {999999ULL, 999ULL}, {1000000ULL, 1000ULL},
            {17179869184ULL, 131072ULL},            /* 2^34, the bad value */
            {28133137871ULL, 167729ULL},            /* live-run variance */
            {4294967295ULL, 65535ULL},
            {4611686018427387904ULL, 2147483648ULL},/* 2^62 */
            {18446744065119617025ULL, 4294967295ULL},/* (2^32-1)^2 */
            {18446744073709551615ULL, 4294967295ULL} /* u64 max */
        };
        unsigned int k;
        int sq_ok = 1;
        for (k = 0; k < sizeof(sq) / sizeof(sq[0]); k++) {
            if (rc_isqrt64(sq[k].x) != sq[k].r) {
                char bx[24], bg[24], bw[24];
                printf("        rc_isqrt64(%s) = %s, want %s\n",
                       rc_u64s(sq[k].x, bx), rc_u64s(rc_isqrt64(sq[k].x), bg),
                       rc_u64s(sq[k].r, bw));
                sq_ok = 0;
            }
        }
        check("isqrt64 table (16 cases)", sq_ok);
        check("live-run stdev is 167729 not 2^34",
              rc_isqrt64(28133137871ULL) == 167729ULL &&
              rc_isqrt64(28133137871ULL) != 17179869184ULL);
    }

    printf("composed pipeline at live-run magnitudes (cv must be ~0.26 not thousands):\n");
    {
        struct rc_entropy e2;
        int k;
        memset(&e2, 0, sizeof(e2));
        e2.sample_count = RC_ENT_SAMPLES;
        e2.timer_ok = 1;
        for (k = 0; k < RC_ENT_SAMPLES; k++)
            e2.samples_ns[k] = (k % 2) ? 1200000UL : 700000UL;
        rc_entropy_stats(&e2);
        check("mean 950000", e2.mean_ns == 950000ULL);
        check("variance 62500000000", e2.variance_ns == 62500000000ULL);
        check("stdev 250000", e2.stdev_ns == 250000ULL);
        check("cv_ppm 263157", e2.cv_ppm == 263157ULL);
        check("drift_stdev 498887", e2.drift_stdev_ns == 498887ULL);
    }
    printf("build sanity: sizeof(long)=%d sizeof(void*)=%d sizeof(long long)=%d\n",
           (int)sizeof(long), (int)sizeof(void *), (int)sizeof(long long));
    check("mean_ns 1059625", ent.mean_ns == 1059625ULL);
    check("variance_ns 6712609375 (would be negative in 32-bit)",
          ent.variance_ns == 6712609375ULL);
    check("stdev_ns 81930", ent.stdev_ns == 81930ULL);
    check("cv_ppm 77319", ent.cv_ppm == 77319UL);
    check("drift_stdev_ns 142080", ent.drift_stdev_ns == 142080ULL);
    check("min/max consistent", ent.min_ns == 951000UL && ent.max_ns == 1210000UL);
    check("clock_drift passes on real-looking samples", rc_clock_drift_passed(&ent));

    printf("JSON payload:\n");
    rc_emu_detect(&emu);
    plen = rc_attest_build_payload(payload, sizeof(payload), &hw,
                                   "amiga-fsuae-scott", "amiga-fsuae-scott",
                                   "cafebabe1234", &ent, &emu);
    check("payload builds", plen > 0);
    check("payload length sane", plen > 400 && plen < RC_PAYLOAD_MAX);
    check("has miner", strstr(payload, "\"miner\":\"amiga-fsuae-scott\"") != NULL);
    check("has miner_id", strstr(payload, "\"miner_id\":\"amiga-fsuae-scott\"") != NULL);
    check("has nonce", strstr(payload, "\"nonce\":\"cafebabe1234\"") != NULL);
    check("has family m68k", strstr(payload, "\"family\":\"m68k\"") != NULL);
    check("has arch 68030", strstr(payload, "\"arch\":\"68030\"") != NULL);
    check("has model", strstr(payload, "\"model\":\"Amiga (AROS/FS-UAE)\"") != NULL);
    check("has rom_hash", strstr(payload, "\"rom_hash\":\"aabd0895") != NULL);
    check("has rom_size", strstr(payload, "\"rom_size\":524288") != NULL);
    check("has rom_checksum", strstr(payload, "\"rom_checksum\":66846720") != NULL);
    check("has attn_flags", strstr(payload, "\"attn_flags\":55") != NULL);
    check("has kick_version", strstr(payload, "\"kick_version\":\"47.96\"") != NULL);
    check("has commitment", strstr(payload, "\"commitment\":\"") != NULL);
    check("has derived entropy", strstr(payload, "\"mean_ns\":1059625") != NULL);
    check("has 64-bit variance", strstr(payload, "\"variance_ns\":6712609375") != NULL);
    check("no negative numbers anywhere", strstr(payload, ":-") == NULL);
    check("has empty macs", strstr(payload, "\"macs\":[]") != NULL);

    printf("fingerprint block:\n");
    check("has fingerprint", strstr(payload, "\"fingerprint\":{") != NULL);
    check("has checks", strstr(payload, "\"checks\":{") != NULL);
    check("clock_drift present", strstr(payload, "\"clock_drift\":{") != NULL);
    check("clock_drift passed", strstr(payload, "\"clock_drift\":{\"passed\":true") != NULL);
    check("cv fixed-point 0.077319", strstr(payload, "\"cv\":0.077319") != NULL);
    check("drift_stdev present", strstr(payload, "\"drift_stdev\":142080") != NULL);
    check("raw samples as evidence",
          strstr(payload, "\"samples_ns\":[1000000,1103000,951000,") != NULL);
    check("anti_emulation present", strstr(payload, "\"anti_emulation\":{") != NULL);
    check("anti_emulation failed (UAE stub)",
          strstr(payload, "\"anti_emulation\":{\"passed\":false") != NULL);
    check("uae.resource indicator",
          strstr(payload, "\"vm_indicators\":[\"uae.resource\"]") != NULL);
    check("all_passed false", strstr(payload, "\"all_passed\":false") != NULL);

    printf("fingerprint block on real hardware (no UAE):\n");
    {
        struct rc_emu real_emu;
        static char p2[RC_PAYLOAD_MAX];
        memset(&real_emu, 0, sizeof(real_emu));
        real_emu.passed = 1;
        check("payload builds", rc_attest_build_payload(p2, sizeof(p2), &hw,
              "real-amiga", "real-amiga", "n0nce", &ent, &real_emu) > 0);
        check("anti_emulation passes",
              strstr(p2, "\"anti_emulation\":{\"passed\":true") != NULL);
        check("empty vm_indicators", strstr(p2, "\"vm_indicators\":[]") != NULL);
        check("all_passed true", strstr(p2, "\"all_passed\":true") != NULL);
    }

    printf("JSON escaping:\n");
    {
        char esc[128];
        rc_json_escape(esc, sizeof(esc), "bad\"wallet\\x\n");
        check_str("escape quotes/backslash/ctrl", esc, "bad\\\"wallet\\\\x\\u000a");
    }

    printf("JSON builder:\n");
    {
        struct rc_jb b;
        char buf[256];
        rc_jb_init(&b, buf, sizeof(buf));
        rc_jb_obj_open(&b, NULL);
        rc_jb_str(&b, "a", "x\"y");
        rc_jb_int(&b, "n", 123);
        rc_jb_u64(&b, "u", 18446744073709551615ULL);
        rc_jb_ulong(&b, "lu", 4294967295UL);
        rc_jb_arr_open(&b, "arr");
        rc_jb_int(&b, NULL, 1);
        rc_jb_int(&b, NULL, 2);
        rc_jb_arr_close(&b);
        rc_jb_obj_open(&b, "o");
        rc_jb_bool(&b, "b", 1);
        rc_jb_obj_close(&b);
        rc_jb_raw(&b, "r", "[3,4]");
        rc_jb_obj_close(&b);
        check("builder ok", rc_jb_ok(&b));
        check_str("builder output", buf,
            "{\"a\":\"x\\\"y\",\"n\":123,\"u\":18446744073709551615,"
            "\"lu\":4294967295,\"arr\":[1,2],\"o\":{\"b\":true},\"r\":[3,4]}");
    }
    {
        struct rc_jb b;
        char tiny[8];
        rc_jb_init(&b, tiny, sizeof(tiny));
        rc_jb_obj_open(&b, NULL);
        rc_jb_str(&b, "key-too-long-to-fit", "value");
        rc_jb_obj_close(&b);
        check("builder overflow flagged", !rc_jb_ok(&b));
        check("builder overflow still terminated",
              strlen(tiny) < sizeof(tiny));
    }

    printf("fail closed: oversized builds error out, nothing truncated:\n");
    {
        static char small[256];
        int r;
        memset(small, 'X', sizeof(small));
        r = rc_attest_build_payload(small, sizeof(small), &hw,
                                    "amiga-fsuae-scott", "amiga-fsuae-scott",
                                    "cafebabe1234", &ent, &emu);
        check("payload in 256 bytes returns -1", r == -1);
        check("small buffer still NUL-terminated",
              memchr(small, '\0', sizeof(small)) != NULL);
        r = rc_attest_build_payload(small, 1, &hw, "w", "m", "n", &ent, &emu);
        check("payload in 1 byte returns -1", r == -1);
    }
    {
        char tinyreq[64];
        check("POST into 64 bytes returns -1",
              rc_http_format_post(tinyreq, sizeof(tinyreq), "50.28.86.131",
                                  8088, "/attest/submit", payload) == -1);
        check("GET into 64 bytes returns -1",
              rc_http_format_get(tinyreq, sizeof(tinyreq), "50.28.86.131",
                                 8088, "/health") == -1);
        check("GET with huge path returns -1",
              rc_http_format_get(req, 300, "h",
                                 80, payload /* 2KB+ of junk path */) == -1);
    }

    printf("JSON payload is parseable (paranoia brace count):\n");
    {
        int depth = 0, i, instr = 0, bad = 0;
        for (i = 0; i < plen; i++) {
            char ch = payload[i];
            if (instr) {
                if (ch == '\\') i++;
                else if (ch == '"') instr = 0;
            } else {
                if (ch == '"') instr = 1;
                else if (ch == '{') depth++;
                else if (ch == '}') depth--;
                if (depth < 0) bad = 1;
            }
        }
        check("braces balance", depth == 0 && !bad && !instr);
    }

    printf("HTTP formatting:\n");
    rlen = rc_http_format_post(req, sizeof(req), "50.28.86.131", 8088,
                               "/attest/submit", payload);
    check("request builds", rlen > 0);
    check("starts with POST", strncmp(req, "POST /attest/submit HTTP/1.1\r\n", 30) == 0);
    check("has host header", strstr(req, "Host: 50.28.86.131:8088\r\n") != NULL);
    {
        char want[64];
        sprintf(want, "Content-Length: %d\r\n", plen);
        check("content-length matches body", strstr(req, want) != NULL);
    }
    check("header/body split", strstr(req, "\r\n\r\n{") != NULL);

    rlen = rc_http_format_get(req, sizeof(req), "50.28.86.131", 8088, "/health");
    check("GET builds", rlen > 0);
    check("starts with GET", strncmp(req, "GET /health HTTP/1.1\r\n", 22) == 0);
    check("GET has host header", strstr(req, "Host: 50.28.86.131:8088\r\n") != NULL);
    check("GET ends with blank line",
          strcmp(req + rlen - 4, "\r\n\r\n") == 0);

    printf("HTTP response parsing:\n");
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
            "{\"nonce\":\"deadbeef\",\"expires_at\":123}";
        const char *body;
        check("status 200", rc_http_status(resp) == 200);
        check("nonce extract", rc_json_find_string(resp, "nonce", nonce_out,
              sizeof(nonce_out)) && strcmp(nonce_out, "deadbeef") == 0);
        body = rc_http_body(resp);
        check("body pointer", body != NULL && body[0] == '{');
    }
    {
        const char *resp412 = "HTTP/1.1 412 Precondition Failed\r\n\r\n{}";
        check("status 412", rc_http_status(resp412) == 412);
        check("not-http is -1", rc_http_status("garbage") == -1);
    }

    printf("host network stubs fail cleanly:\n");
    {
        static char resp[64];
        check("rc_http_open is 0 on host", rc_http_open() == 0);
        check("rc_http_get is -1 on host",
              rc_http_get("127.0.0.1", 1, "/", resp, sizeof(resp)) == -1);
    }

    printf("\n%s (%d failures)\n",
           failures ? "SMOKE TEST FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 20 : 0;
}
