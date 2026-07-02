/*
 * rc_attest.c - attestation payload and challenge/submit flow.
 *
 * Extracted from the working miner (rustchain_amiga_miner.c): same
 * field names, same order, same formatting. The host test proves the
 * built payload is byte-identical to the miner's. Field names match
 * rustchain_mac_universal_miner_v2.2.2.py plus rom_hash/rom_size/
 * kick_version in device for server ROM clustering. Server accepts
 * both model|arch|family and device_* names (2025-12-20 fix).
 *
 * One deliberate upgrade over the miner: the payload is built with the
 * bounds-checked rc_jb builder instead of raw sprintf, so an oversized
 * build fails closed (returns -1, nothing gets sent) instead of
 * overflowing or silently truncating JSON on the wire.
 */

#include <stdio.h>
#include <string.h>

#include "rustchain/rc_attest.h"
#include "rustchain/rc_http.h"
#include "rustchain/rc_json.h"
#include "rustchain/rc_sha1.h"
#include "rustchain/rc_u64.h"

/*
 * Fingerprint block, shape from fingerprint_checks.py / the linux miner:
 * {"all_passed":bool,"checks":{name:{"passed":bool,"data":{...}}}}
 * Evidence over verdicts: raw EClock samples go in clock_drift.data,
 * anti_emulation lists the concrete indicators found (uae.resource etc).
 */
static void jb_fingerprint(struct rc_jb *b,
                           const struct rc_entropy *e,
                           const struct rc_emu *em)
{
    char cvbuf[40], whole[24];
    int clock_ok = rc_clock_drift_passed(e);
    int i;

    /* cv as fixed point W.FFFFFF from cv_ppm, so the server's
       cv < 0.0001 synthetic-timing check reads it as a normal float */
    sprintf(cvbuf, "%s.%06lu",
            rc_u64s(e->cv_ppm / 1000000ULL, whole),
            (unsigned long)(e->cv_ppm % 1000000ULL));

    rc_jb_obj_open(b, "fingerprint");
    rc_jb_bool(b, "all_passed", clock_ok && em->passed);
    rc_jb_obj_open(b, "checks");

    rc_jb_obj_open(b, "clock_drift");
    rc_jb_bool(b, "passed", clock_ok);
    rc_jb_obj_open(b, "data");
    rc_jb_u64(b, "mean_ns", e->mean_ns);
    rc_jb_u64(b, "stdev_ns", e->stdev_ns);
    rc_jb_raw(b, "cv", cvbuf);
    rc_jb_u64(b, "drift_stdev", e->drift_stdev_ns);
    rc_jb_str(b, "timer_source", e->timer_ok ? "eclock" : "none");
    rc_jb_arr_open(b, "samples_ns");
    for (i = 0; i < e->sample_count; i++)
        rc_jb_ulong(b, NULL, e->samples_ns[i]);
    rc_jb_arr_close(b);
    rc_jb_obj_close(b);   /* data */
    rc_jb_obj_close(b);   /* clock_drift */

    rc_jb_obj_open(b, "anti_emulation");
    rc_jb_bool(b, "passed", em->passed);
    rc_jb_obj_open(b, "data");
    rc_jb_str(b, "platform", "amigaos");
    rc_jb_arr_open(b, "vm_indicators");
    if (em->uae_resource)
        rc_jb_str(b, NULL, "uae.resource");
    if (em->uae_id_string)
        rc_jb_str(b, NULL, "bsdsocket_id:uae");
    rc_jb_arr_close(b);
    rc_jb_obj_close(b);   /* data */
    rc_jb_obj_close(b);   /* anti_emulation */

    rc_jb_obj_close(b);   /* checks */
    rc_jb_obj_close(b);   /* fingerprint */
}

/*
 * Attestation payload. macs is left empty on purpose: unknown emulated
 * OUIs trip the RIP-0147a gate. commitment is opaque to the server
 * (stored as-is), sha1 here over the escaped nonce + wallet plus arch
 * and rom_hash, exactly as the miner computes it.
 */
int rc_attest_build_payload(char *out, int outlen,
                            const struct rc_hwinfo *hw,
                            const char *wallet, const char *miner_id,
                            const char *nonce,
                            const struct rc_entropy *e,
                            const struct rc_emu *em)
{
    struct rc_jb b;
    char w[128], n[160];
    char commit_src[512];
    char commitment[41];

    rc_json_escape(w, sizeof(w), wallet);
    rc_json_escape(n, sizeof(n), nonce);
    sprintf(commit_src, "%s%s%s%s", n, w, hw->arch, hw->rom_hash);
    rc_sha1_hex((const unsigned char *)commit_src,
                (unsigned long)strlen(commit_src), commitment);

    rc_jb_init(&b, out, outlen);
    rc_jb_obj_open(&b, NULL);
    rc_jb_str(&b, "miner", wallet);
    rc_jb_str(&b, "miner_id", miner_id);
    rc_jb_str(&b, "nonce", nonce);

    rc_jb_obj_open(&b, "report");
    rc_jb_str(&b, "nonce", nonce);
    rc_jb_str(&b, "commitment", commitment);
    rc_jb_obj_open(&b, "derived");
    rc_jb_u64(&b, "mean_ns", e->mean_ns);
    rc_jb_u64(&b, "variance_ns", e->variance_ns);
    rc_jb_ulong(&b, "min_ns", e->min_ns);
    rc_jb_ulong(&b, "max_ns", e->max_ns);
    rc_jb_int(&b, "sample_count", e->sample_count);
    rc_jb_obj_close(&b);  /* derived */
    rc_jb_u64(&b, "entropy_score", e->variance_ns);
    rc_jb_obj_close(&b);  /* report */

    rc_jb_obj_open(&b, "device");
    rc_jb_str(&b, "family", "m68k");
    rc_jb_str(&b, "arch", hw->arch);
    rc_jb_str(&b, "model", "Amiga (AROS/FS-UAE)");
    rc_jb_str(&b, "cpu", hw->cpu);
    rc_jb_str(&b, "fpu", hw->fpu);
    rc_jb_int(&b, "cores", 1);
    /* integer 0 on purpose: no floats in the payload, memory_kb is real */
    rc_jb_int(&b, "memory_gb", 0);
    rc_jb_ulong(&b, "memory_kb", hw->mem_kb);
    rc_jb_str(&b, "machine", "m68k");
    rc_jb_ulong(&b, "attn_flags", hw->attn_flags);
    rc_jb_str(&b, "rom_hash", hw->rom_hash);
    rc_jb_ulong(&b, "rom_size", hw->rom_size);
    rc_jb_ulong(&b, "rom_checksum", hw->rom_checksum);
    rc_jb_str(&b, "kick_version", hw->kick_version);
    rc_jb_str(&b, "miner_version", RC_CLIENT_VERSION);
    rc_jb_obj_close(&b);  /* device */

    rc_jb_obj_open(&b, "signals");
    rc_jb_arr_open(&b, "macs");
    rc_jb_arr_close(&b);
    rc_jb_str(&b, "hostname", "amiga-fsuae");
    rc_jb_obj_close(&b);  /* signals */

    jb_fingerprint(&b, e, em);
    rc_jb_obj_close(&b);  /* root */

    /* fail closed: a payload that did not fit is never worth sending */
    if (!rc_jb_ok(&b))
        return -1;
    return b.len;
}

#ifndef HOST_TEST

int rc_attest_once(const char *host, int port,
                   const char *wallet, const char *miner_id,
                   const struct rc_hwinfo *hw)
{
    static char resp[RC_RESP_MAX];
    static char payload[RC_PAYLOAD_MAX];
    char nonce[160];
    struct rc_entropy ent;
    struct rc_emu emu;
    int n, status;

    printf("[ATTEST] requesting challenge from %s:%d\n", host, port);
    n = rc_http_post(host, port, "/attest/challenge", "{}", resp, sizeof(resp));
    if (n <= 0) return 0;

    status = rc_http_status(resp);
    if (status != 200) {
        printf("[FAIL] challenge HTTP %d\n", status);
        return 0;
    }
    if (!rc_json_find_string(resp, "nonce", nonce, sizeof(nonce))) {
        printf("[FAIL] no nonce in challenge response\n");
        return 0;
    }
    printf("[OK] got nonce\n");

    rc_entropy_collect(&ent);
    rc_emu_detect(&emu);
    if (!emu.passed)
        printf("[INFO] UAE detected, reporting honestly (flagging expected)\n");
    if (rc_attest_build_payload(payload, sizeof(payload), hw, wallet, miner_id,
                                nonce, &ent, &emu) < 0) {
        printf("[FAIL] payload too large\n");
        return 0;
    }

    printf("[ATTEST] submitting (%d bytes)\n", (int)strlen(payload));
    n = rc_http_post(host, port, "/attest/submit", payload, resp, sizeof(resp));
    if (n <= 0) return 0;

    status = rc_http_status(resp);
    if (status == 200 && strstr(resp, "\"ok\"")) {
        char ticket[64];
        printf("[PASS] attestation accepted\n");
        if (rc_json_find_string(resp, "ticket_id", ticket, sizeof(ticket)))
            printf("       ticket: %s\n", ticket);
        if (strstr(resp, "\"fingerprint_passed\":false") ||
            strstr(resp, "\"fingerprint_passed\": false"))
            printf("       flagged: emulated hardware, minimal reward (by design)\n");
        return 1;
    }

    printf("[FAIL] submit HTTP %d\n", status);
    /* show start of body so failures are debuggable from a log file */
    {
        const char *body = rc_http_body(resp);
        if (body) {
            char snip[201];
            strncpy(snip, body, 200);
            snip[200] = '\0';
            printf("       %s\n", snip);
        }
    }
    return 0;
}

#endif /* !HOST_TEST */
