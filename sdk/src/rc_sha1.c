/*
 * rc_sha1.c - small self-contained SHA-1.
 * Extracted unchanged from the working miner (rustchain_amiga_miner.c).
 * Verified against NIST vectors in the host test.
 */

#include <string.h>

#include "rustchain/rc_sha1.h"

static unsigned long sha1_rol(unsigned long v, int n)
{
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFFUL;
}

void rc_sha1_init(rc_sha1_ctx *c)
{
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->buf_used = 0;
}

static void sha1_block(rc_sha1_ctx *c, const unsigned char *p)
{
    unsigned long w[80];
    unsigned long a, b, d, e, f, k, t;
    unsigned long cc;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)p[i * 4] << 24) |
               ((unsigned long)p[i * 4 + 1] << 16) |
               ((unsigned long)p[i * 4 + 2] << 8) |
               ((unsigned long)p[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b) & d);          k = 0x5A827999UL; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1UL; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCUL; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6UL; }
        t = (sha1_rol(a, 5) + f + e + k + w[i]) & 0xFFFFFFFFUL;
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
    }

    c->h[0] = (c->h[0] + a) & 0xFFFFFFFFUL;
    c->h[1] = (c->h[1] + b) & 0xFFFFFFFFUL;
    c->h[2] = (c->h[2] + cc) & 0xFFFFFFFFUL;
    c->h[3] = (c->h[3] + d) & 0xFFFFFFFFUL;
    c->h[4] = (c->h[4] + e) & 0xFFFFFFFFUL;
}

void rc_sha1_update(rc_sha1_ctx *c, const unsigned char *p, unsigned long n)
{
    unsigned long old = c->len_lo;
    c->len_lo = (c->len_lo + (n << 3)) & 0xFFFFFFFFUL;
    if (c->len_lo < old)
        c->len_hi++;
    c->len_hi += (n >> 29);

    while (n > 0) {
        unsigned long take = 64 - c->buf_used;
        if (take > n) take = n;
        memcpy(c->buf + c->buf_used, p, take);
        c->buf_used += (int)take;
        p += take;
        n -= take;
        if (c->buf_used == 64) {
            sha1_block(c, c->buf);
            c->buf_used = 0;
        }
    }
}

void rc_sha1_final(rc_sha1_ctx *c, unsigned char out[20])
{
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenb[8];
    unsigned long lo = c->len_lo, hi = c->len_hi;
    int i;

    rc_sha1_update(c, &pad, 1);
    while (c->buf_used != 56)
        rc_sha1_update(c, &zero, 1);

    lenb[0] = (unsigned char)(hi >> 24); lenb[1] = (unsigned char)(hi >> 16);
    lenb[2] = (unsigned char)(hi >> 8);  lenb[3] = (unsigned char)(hi);
    lenb[4] = (unsigned char)(lo >> 24); lenb[5] = (unsigned char)(lo >> 16);
    lenb[6] = (unsigned char)(lo >> 8);  lenb[7] = (unsigned char)(lo);
    /* length goes in raw, do not recount it */
    memcpy(c->buf + 56, lenb, 8);
    sha1_block(c, c->buf);

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

void rc_sha1_hex(const unsigned char *data, unsigned long n, char out[41])
{
    rc_sha1_ctx c;
    unsigned char dig[20];
    int i;
    static const char hexd[] = "0123456789abcdef";

    rc_sha1_init(&c);
    rc_sha1_update(&c, data, n);
    rc_sha1_final(&c, dig);
    for (i = 0; i < 20; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0x0F];
    }
    out[40] = '\0';
}
