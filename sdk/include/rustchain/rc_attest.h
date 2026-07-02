/*
 * rc_attest.h - RustChain attestation client.
 *
 * Protocol matches rustchain_mac_universal_miner_v2.2.2.py, the legacy
 * plain-HTTP path (same as the G4/G5 miners). No Ed25519.
 *
 * Flow: POST /attest/challenge {} then take "nonce" from the reply and
 * POST /attest/submit with miner/miner_id/nonce/report/device/signals/
 * fingerprint.
 *
 * The fingerprint block is honest, evidence over verdicts: clock_drift
 * carries the raw EClock samples, anti_emulation lists the concrete
 * indicators found (uae.resource etc). Emulated hardware is expected
 * to be flagged server side and that is fine.
 *
 * Wire details that must not drift (see miner/README.md for the full
 * rationale):
 *   - commitment is SHA-1 of nonce + wallet + arch + rom_hash, the
 *     server stores it as an opaque string
 *   - macs is an empty list on purpose, an emulated NIC OUI would trip
 *     the RIP-0147a gate
 *   - memory_gb is integer 0 on small machines, memory_kb carries the
 *     real number, no floats anywhere in the payload
 *   - cv is printed as fixed point W.FFFFFF from cv_ppm so the server
 *     reads it as a normal float
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_ATTEST_H
#define RC_ATTEST_H

#include "rc_hw.h"

#define RC_CLIENT_VERSION "1.0"

#define RC_DEFAULT_NODE_HOST "50.28.86.131"
#define RC_DEFAULT_NODE_PORT 8088
#define RC_PAYLOAD_MAX 4096

/*
 * Build the full /attest/submit JSON payload into out.
 * Returns the length, or -1 if it did not fit.
 * Portable, covered by the host test.
 */
int rc_attest_build_payload(char *out, int outlen,
                            const struct rc_hwinfo *hw,
                            const char *wallet, const char *miner_id,
                            const char *nonce,
                            const struct rc_entropy *e,
                            const struct rc_emu *em);

/*
 * One full challenge/submit round against the node. Collects entropy
 * and runs emulation detection itself. Prints progress to stdout the
 * same way the standalone miner does. Returns 1 if the attestation
 * was accepted (even when flagged as emulated), 0 otherwise.
 * Amiga-only; requires rc_http_open() first.
 */
int rc_attest_once(const char *host, int port,
                   const char *wallet, const char *miner_id,
                   const struct rc_hwinfo *hw);

#endif /* RC_ATTEST_H */
