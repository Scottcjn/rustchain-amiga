/*
 * rc_sha1.h - small self-contained SHA-1.
 *
 * No external crypto lib. Used for the ROM fingerprint (server ROM
 * clustering DB stores lowercase SHA-1 hex) and the attestation
 * commitment. Verified against the NIST vectors in the host test.
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_SHA1_H
#define RC_SHA1_H

typedef struct {
    unsigned long h[5];
    unsigned long len_lo;
    unsigned long len_hi;
    unsigned char buf[64];
    int buf_used;
} rc_sha1_ctx;

void rc_sha1_init(rc_sha1_ctx *c);
void rc_sha1_update(rc_sha1_ctx *c, const unsigned char *p, unsigned long n);
void rc_sha1_final(rc_sha1_ctx *c, unsigned char out[20]);

/* one-shot: hash n bytes, write 40 lowercase hex chars plus NUL */
void rc_sha1_hex(const unsigned char *data, unsigned long n, char out[41]);

#endif /* RC_SHA1_H */
