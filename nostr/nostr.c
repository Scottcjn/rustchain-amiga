/*
 * nostr.c - a native Nostr client for classic AmigaOS (m68k)
 *
 * Connects to a Nostr relay over a WebSocket, subscribes for notes
 * (kind 1 events), and prints each note's content and author pubkey.
 *
 * WHAT WORKS
 *   READ-ONLY (subscribe + display). The client:
 *     1. opens a TCP socket (bsdsocket.library), optionally wrapped in TLS
 *        via AmiSSL for wss:// relays;
 *     2. performs the RFC6455 WebSocket client handshake (HTTP Upgrade,
 *        Sec-WebSocket-Key / verified Sec-WebSocket-Accept);
 *     3. speaks RFC6455 framing (client->server frames are masked, as the
 *        spec requires; server->client frames are read unmasked);
 *     4. sends a Nostr REQ  ["REQ","sub1",{"kinds":[1],"limit":N}]
 *        and reads incoming ["EVENT","sub1",{...}] messages, printing each
 *        note's content and author pubkey. ["EOSE",...] is handled.
 *
 *   PUBLISH is NOT implemented. A Nostr note must be signed with a
 *   secp256k1 Schnorr signature (BIP340) over the event id, and a full
 *   secp256k1 in C on m68k is a large undertaking that this client does
 *   not fake. What IS implemented and host-tested is the easy, testable
 *   half of publishing: the NIP-01 event serialization and the sha256
 *   event-id computation (see --publish). The signing step is left as a
 *   clearly marked TODO.
 *
 * Two transports (like the sibling claude/ client):
 *   - wss:// : AmiSSL direct TLS on 443 (needs AmiSSL + bsdsocket in-guest).
 *   - ws://  : plain TCP, no TLS. Testable against a local mock relay with
 *              no AmiSSL at all. --proxy host:port overrides the TCP
 *              endpoint (plaintext) so a host-side proxy can terminate TLS.
 *
 * m68k rules (carried from claude/client/claude.c and miner/):
 *   - Build with -m68020 (NOT -m68000: bebbo miscompiles __mulsi3/atoi at
 *     -m68000 and HANGS). Avoid runtime-variable 32-bit multiplies; the
 *     hashes here use only shifts/adds/xor.
 *   - C89-friendly: declarations first, no // comments, no C99 loop decls.
 *   - Big buffers are static, never on the stack (TLS is stack-hungry).
 *   - No libnix "unsigned long __stack" global (CloseLibrary hangs at exit).
 *   - fflush(stdout) after prints; bsdsocket opened v3 (via rtc_net_open).
 *
 * The pure logic (sha1, sha256, base64, WebSocket frame encode/decode,
 * Nostr JSON parsing and event serialization) compiles for the host too
 * and is exercised by -DHOST_TEST (see `make host-test`).
 *
 * Build:
 *   Amiga hunk exe : see Makefile target `nostr` (docker cross + AmiSSL)
 *   Host self-test : -DHOST_TEST (see Makefile target `host-test`)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/rtc_common.h"

/* -------- sizes (static buffers) -------- */
#define WS_GUID    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define INBUF      72000     /* raw receive buffer (must hold one full frame) */
#define PAYBUF     66000     /* decoded frame payload cap */
#define MSGBUF     131072    /* assembled WebSocket message (fragments joined) */
#define SENDBUF    8192      /* outgoing frame buffer */
#define OBJBUF     66000     /* extracted Nostr event object */
#define SERBUF     70000     /* NIP-01 event serialization */

/* ================================================================== */
/* SHA-1 (raw 20-byte digest; needed for Sec-WebSocket-Accept)        */
/* 32-bit-clean: all state masked to 32 bits so a 64-bit host is safe */
/* ================================================================== */

#define M32 0xFFFFFFFFUL

typedef struct {
    unsigned long h[5];
    unsigned long lo, hi;      /* bit length, low/high 32 bits */
    unsigned char buf[64];
    int used;
} sha1_ctx;

static unsigned long rol32(unsigned long v, int n)
{
    return ((v << n) | ((v & M32) >> (32 - n))) & M32;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301UL; c->h[1] = 0xEFCDAB89UL; c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL; c->h[4] = 0xC3D2E1F0UL;
    c->lo = c->hi = 0; c->used = 0;
}

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    unsigned long w[80];
    unsigned long a, b, cc, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((unsigned long)p[i*4] << 24) | ((unsigned long)p[i*4+1] << 16) |
               ((unsigned long)p[i*4+2] << 8) | (unsigned long)p[i*4+3];
    for (i = 16; i < 80; i++)
        w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b & M32) & d);  k = 0x5A827999UL; }
        else if (i < 40) { f = b ^ cc ^ d;                   k = 0x6ED9EBA1UL; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCUL; }
        else             { f = b ^ cc ^ d;                   k = 0xCA62C1D6UL; }
        t = (rol32(a, 5) + f + e + k + w[i]) & M32;
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] = (c->h[0] + a) & M32; c->h[1] = (c->h[1] + b) & M32;
    c->h[2] = (c->h[2] + cc) & M32; c->h[3] = (c->h[3] + d) & M32;
    c->h[4] = (c->h[4] + e) & M32;
}

static void sha1_update(sha1_ctx *c, const unsigned char *p, unsigned long n)
{
    unsigned long old = c->lo;
    c->lo = (c->lo + (n << 3)) & M32;
    if (c->lo < old) c->hi = (c->hi + 1) & M32;
    c->hi = (c->hi + (n >> 29)) & M32;
    while (n > 0) {
        unsigned long take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += (int)take; p += take; n -= take;
        if (c->used == 64) { sha1_block(c, c->buf); c->used = 0; }
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    unsigned char pad = 0x80, zero = 0, lenb[8];
    unsigned long lo = c->lo, hi = c->hi;
    int i;

    sha1_update(c, &pad, 1);
    while (c->used != 56) sha1_update(c, &zero, 1);
    lenb[0]=(unsigned char)(hi>>24); lenb[1]=(unsigned char)(hi>>16);
    lenb[2]=(unsigned char)(hi>>8);  lenb[3]=(unsigned char)hi;
    lenb[4]=(unsigned char)(lo>>24); lenb[5]=(unsigned char)(lo>>16);
    lenb[6]=(unsigned char)(lo>>8);  lenb[7]=(unsigned char)lo;
    memcpy(c->buf + 56, lenb, 8);
    sha1_block(c, c->buf);
    for (i = 0; i < 5; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void sha1_raw(const unsigned char *data, unsigned long n,
                     unsigned char out[20])
{
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, n);
    sha1_final(&c, out);
}

/* ================================================================== */
/* SHA-256 (raw 32-byte digest; needed for the Nostr event id)        */
/* ================================================================== */

typedef struct {
    unsigned long h[8];
    unsigned long lo, hi;
    unsigned char buf[64];
    int used;
} sha256_ctx;

static const unsigned long K256[64] = {
    0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,0x3956c25bUL,0x59f111f1UL,
    0x923f82a4UL,0xab1c5ed5UL,0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,
    0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,0xe49b69c1UL,0xefbe4786UL,
    0x0fc19dc6UL,0x240ca1ccUL,0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
    0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,0xc6e00bf3UL,0xd5a79147UL,
    0x06ca6351UL,0x14292967UL,0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,
    0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,0xa2bfe8a1UL,0xa81a664bUL,
    0xc24b8b70UL,0xc76c51a3UL,0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
    0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,0x391c0cb3UL,0x4ed8aa4aUL,
    0x5b9cca4fUL,0x682e6ff3UL,0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
    0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
};

static unsigned long ror32(unsigned long v, int n)
{
    return (((v & M32) >> n) | (v << (32 - n))) & M32;
}

static void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667UL; c->h[1]=0xbb67ae85UL; c->h[2]=0x3c6ef372UL;
    c->h[3]=0xa54ff53aUL; c->h[4]=0x510e527fUL; c->h[5]=0x9b05688cUL;
    c->h[6]=0x1f83d9abUL; c->h[7]=0x5be0cd19UL;
    c->lo = c->hi = 0; c->used = 0;
}

static void sha256_block(sha256_ctx *c, const unsigned char *p)
{
    unsigned long w[64];
    unsigned long a,b,cc,d,e,f,g,hh,s0,s1,ch,maj,t1,t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((unsigned long)p[i*4] << 24) | ((unsigned long)p[i*4+1] << 16) |
               ((unsigned long)p[i*4+2] << 8) | (unsigned long)p[i*4+3];
    for (i = 16; i < 64; i++) {
        s0 = ror32(w[i-15],7) ^ ror32(w[i-15],18) ^ ((w[i-15] & M32) >> 3);
        s1 = ror32(w[i-2],17) ^ ror32(w[i-2],19)  ^ ((w[i-2] & M32) >> 10);
        w[i] = (w[i-16] + s0 + w[i-7] + s1) & M32;
    }
    a=c->h[0];b=c->h[1];cc=c->h[2];d=c->h[3];e=c->h[4];f=c->h[5];g=c->h[6];hh=c->h[7];
    for (i = 0; i < 64; i++) {
        s1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        ch = (e & f) ^ ((~e & M32) & g);
        t1 = (hh + s1 + ch + K256[i] + w[i]) & M32;
        s0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = (s0 + maj) & M32;
        hh=g; g=f; f=e; e=(d+t1)&M32; d=cc; cc=b; b=a; a=(t1+t2)&M32;
    }
    c->h[0]=(c->h[0]+a)&M32; c->h[1]=(c->h[1]+b)&M32; c->h[2]=(c->h[2]+cc)&M32;
    c->h[3]=(c->h[3]+d)&M32; c->h[4]=(c->h[4]+e)&M32; c->h[5]=(c->h[5]+f)&M32;
    c->h[6]=(c->h[6]+g)&M32; c->h[7]=(c->h[7]+hh)&M32;
}

static void sha256_update(sha256_ctx *c, const unsigned char *p, unsigned long n)
{
    unsigned long old = c->lo;
    c->lo = (c->lo + (n << 3)) & M32;
    if (c->lo < old) c->hi = (c->hi + 1) & M32;
    c->hi = (c->hi + (n >> 29)) & M32;
    while (n > 0) {
        unsigned long take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += (int)take; p += take; n -= take;
        if (c->used == 64) { sha256_block(c, c->buf); c->used = 0; }
    }
}

static void sha256_final(sha256_ctx *c, unsigned char out[32])
{
    unsigned char pad = 0x80, zero = 0, lenb[8];
    unsigned long lo = c->lo, hi = c->hi;
    int i;

    sha256_update(c, &pad, 1);
    while (c->used != 56) sha256_update(c, &zero, 1);
    lenb[0]=(unsigned char)(hi>>24); lenb[1]=(unsigned char)(hi>>16);
    lenb[2]=(unsigned char)(hi>>8);  lenb[3]=(unsigned char)hi;
    lenb[4]=(unsigned char)(lo>>24); lenb[5]=(unsigned char)(lo>>16);
    lenb[6]=(unsigned char)(lo>>8);  lenb[7]=(unsigned char)lo;
    memcpy(c->buf + 56, lenb, 8);
    sha256_block(c, c->buf);
    for (i = 0; i < 8; i++) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void sha256_hex(const unsigned char *data, unsigned long n, char out[65])
{
    sha256_ctx c;
    unsigned char dig[32];
    int i;
    static const char hx[] = "0123456789abcdef";
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, dig);
    for (i = 0; i < 32; i++) { out[i*2]=hx[dig[i]>>4]; out[i*2+1]=hx[dig[i]&0x0F]; }
    out[64] = '\0';
}

/* ================================================================== */
/* base64 encode                                                      */
/* ================================================================== */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode inlen bytes into a NUL-terminated base64 string. Returns the
   string length, or -1 if out is too small. */
static int base64_encode(const unsigned char *in, int inlen,
                         char *out, int outcap)
{
    int i = 0, o = 0;
    unsigned int a, b, c, trip;

    while (i < inlen) {
        int rem = inlen - i;
        if (o + 4 >= outcap) return -1;
        a = in[i];
        b = (rem > 1) ? in[i+1] : 0;
        c = (rem > 2) ? in[i+2] : 0;
        trip = (a << 16) | (b << 8) | c;
        out[o++] = B64[(trip >> 18) & 0x3F];
        out[o++] = B64[(trip >> 12) & 0x3F];
        out[o++] = (rem > 1) ? B64[(trip >> 6) & 0x3F] : '=';
        out[o++] = (rem > 2) ? B64[trip & 0x3F] : '=';
        i += 3;
    }
    out[o] = '\0';
    return o;
}

/* Compute the Sec-WebSocket-Accept value for a given key string. */
static void ws_accept_key(const char *key_b64, char *out, int outcap)
{
    static char joined[128];
    unsigned char dig[20];
    int n;

    n = sprintf(joined, "%s%s", key_b64, WS_GUID);
    sha1_raw((const unsigned char *)joined, (unsigned long)n, dig);
    base64_encode(dig, 20, out, outcap);
}

/* ================================================================== */
/* WebSocket RFC6455 frame encode / decode (pure, host-tested)        */
/* ================================================================== */

/* Build a masked client frame. Returns total length or -1 on overflow. */
static int ws_build_frame(unsigned char *out, int outcap,
                          const unsigned char *pay, int plen,
                          int opcode, const unsigned char mask[4])
{
    int h, i;

    if (outcap < plen + 14) return -1;
    out[0] = (unsigned char)(0x80 | (opcode & 0x0F));   /* FIN + opcode */
    if (plen < 126) {
        out[1] = (unsigned char)(0x80 | plen);
        h = 2;
    } else if (plen < 65536) {
        out[1] = (unsigned char)(0x80 | 126);
        out[2] = (unsigned char)((plen >> 8) & 0xFF);
        out[3] = (unsigned char)(plen & 0xFF);
        h = 4;
    } else {
        out[1] = (unsigned char)(0x80 | 127);
        out[2] = out[3] = out[4] = out[5] = 0;
        out[6] = (unsigned char)((plen >> 24) & 0xFF);
        out[7] = (unsigned char)((plen >> 16) & 0xFF);
        out[8] = (unsigned char)((plen >> 8) & 0xFF);
        out[9] = (unsigned char)(plen & 0xFF);
        h = 10;
    }
    out[h] = mask[0]; out[h+1] = mask[1];
    out[h+2] = mask[2]; out[h+3] = mask[3];
    h += 4;
    for (i = 0; i < plen; i++)
        out[h + i] = (unsigned char)(pay[i] ^ mask[i & 3]);
    return h + plen;
}

/* Parse one frame out of a byte buffer. Returns:
     >0  total bytes consumed (header + payload); payload copied (unmasked)
      0  incomplete, need more bytes
     -1  error (unsupported huge length, or payload exceeds paycap) */
static int ws_parse_frame(const unsigned char *in, int inlen,
                          unsigned char *pay, int paycap,
                          int *fin, int *opcode, int *paylen)
{
    int h = 2, masked, i;
    long len;
    unsigned char mk[4];

    if (inlen < 2) return 0;
    *fin = (in[0] & 0x80) ? 1 : 0;
    *opcode = in[0] & 0x0F;
    masked = (in[1] & 0x80) ? 1 : 0;
    len = in[1] & 0x7F;
    if (len == 126) {
        if (inlen < 4) return 0;
        len = ((long)in[2] << 8) | (long)in[3];
        h = 4;
    } else if (len == 127) {
        if (inlen < 10) return 0;
        if (in[2] || in[3] || in[4] || in[5]) return -1;   /* > 4 GB: reject */
        len = ((long)in[6] << 24) | ((long)in[7] << 16) |
              ((long)in[8] << 8) | (long)in[9];
        h = 10;
    }
    if (masked) {
        if (inlen < h + 4) return 0;
        mk[0]=in[h]; mk[1]=in[h+1]; mk[2]=in[h+2]; mk[3]=in[h+3];
        h += 4;
    }
    if (len > (long)paycap) return -1;
    if ((long)inlen < (long)h + len) return 0;
    for (i = 0; i < (int)len; i++) {
        unsigned char b = in[h + i];
        if (masked) b = (unsigned char)(b ^ mk[i & 3]);
        pay[i] = b;
    }
    *paylen = (int)len;
    return h + (int)len;
}

/* ================================================================== */
/* Minimal JSON helpers (vendored from claude/client/claude.c)        */
/* ================================================================== */

static int json_unescape(const char *src, int srclen, char *dst, int dstlen)
{
    int i = 0, o = 0;
    while (i < srclen && o < dstlen - 4) {
        char c = src[i];
        if (c == '\\' && i + 1 < srclen) {
            char n = src[i + 1];
            if (n == 'n') { dst[o++] = '\n'; i += 2; }
            else if (n == 'r') { dst[o++] = '\r'; i += 2; }
            else if (n == 't') { dst[o++] = '\t'; i += 2; }
            else if (n == 'b') { dst[o++] = '\b'; i += 2; }
            else if (n == 'f') { dst[o++] = '\f'; i += 2; }
            else if (n == '/') { dst[o++] = '/'; i += 2; }
            else if (n == '"') { dst[o++] = '"'; i += 2; }
            else if (n == '\\') { dst[o++] = '\\'; i += 2; }
            else if (n == 'u' && i + 5 < srclen) {
                int v = 0, k;
                for (k = 0; k < 4; k++) {
                    char hh = src[i + 2 + k];
                    v <<= 4;
                    if (hh >= '0' && hh <= '9') v |= (hh - '0');
                    else if (hh >= 'a' && hh <= 'f') v |= (hh - 'a' + 10);
                    else if (hh >= 'A' && hh <= 'F') v |= (hh - 'A' + 10);
                }
                if (v < 0x80) dst[o++] = (char)v;
                else if (v < 0x800) {
                    dst[o++] = (char)(0xC0 | (v >> 6));
                    dst[o++] = (char)(0x80 | (v & 0x3F));
                } else {
                    dst[o++] = (char)(0xE0 | (v >> 12));
                    dst[o++] = (char)(0x80 | ((v >> 6) & 0x3F));
                    dst[o++] = (char)(0x80 | (v & 0x3F));
                }
                i += 6;
            } else { dst[o++] = c; i++; }
        } else { dst[o++] = c; i++; }
    }
    dst[o] = '\0';
    return o;
}

static const char *find_key(const char *json, const char *key)
{
    char pat[80];
    const char *p;
    int kl;

    if ((int)strlen(key) > 70) return NULL;
    sprintf(pat, "\"%s\"", key);
    kl = (int)strlen(pat);
    p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + kl;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') return q + 1;
        p += kl;
    }
    return NULL;
}

static int get_json_string(const char *json, const char *key,
                           char *out, int outlen)
{
    const char *p, *vs;
    int vlen, instr;

    if (outlen < 1) return 0;
    out[0] = '\0';
    p = find_key(json, key);
    if (!p) return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;
    vs = p;
    instr = 1;
    while (*p && instr) {
        if (*p == '\\' && p[1]) p += 2;
        else if (*p == '"') { instr = 0; break; }
        else p++;
    }
    vlen = (int)(p - vs);
    json_unescape(vs, vlen, out, outlen);
    return 1;
}

/* Copy the first balanced {...} object in json into out. Returns 1 ok.
   String-aware: braces inside "..." strings do not change nesting, and an
   escaped char inside a string (\" etc.) is copied verbatim. */
static int extract_first_object(const char *json, char *out, int outlen)
{
    const char *p = json;
    int depth = 0, instr = 0, o = 0;

    while (*p && *p != '{') p++;
    if (*p != '{') return 0;
    for (; *p; p++) {
        char ch = *p;
        if (o < outlen - 1) out[o++] = ch;
        if (instr) {
            if (ch == '\\' && p[1]) {
                if (o < outlen - 1) out[o++] = p[1];
                p++;
            } else if (ch == '"') {
                instr = 0;
            }
        } else {
            if (ch == '"') instr = 1;
            else if (ch == '{') depth++;
            else if (ch == '}') {
                depth--;
                if (depth == 0) { out[o] = '\0'; return 1; }
            }
        }
    }
    out[o] = '\0';
    return 0;
}

/* Return the first double-quoted string in a JSON array element list into
   out (e.g. the "EVENT"/"EOSE"/"NOTICE" tag). Returns 1 ok. */
static int first_array_string(const char *json, char *out, int outlen)
{
    const char *p = json;
    int o = 0;

    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    while (*p && *p != '"') {
        if (*p == '{' || *p == '[') return 0;   /* no leading string */
        p++;
    }
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && o < outlen - 1) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 1;
}

/* ================================================================== */
/* Nostr NIP-01 event serialization + event id (host-tested)          */
/* ================================================================== */

/* NIP-01 content escaping: only \" \\ \n \r \t \b \f are escaped; every
   other byte (including UTF-8 continuation bytes) is emitted verbatim.
   Returns escaped length, or -1 on overflow. */
static int nostr_escape(char *dst, int cap, const char *src)
{
    const unsigned char *s = (const unsigned char *)src;
    int o = 0;

    for (; *s; s++) {
        unsigned char c = *s;
        if (o >= cap - 2) return -1;
        switch (c) {
            case '"':  dst[o++]='\\'; dst[o++]='"'; break;
            case '\\': dst[o++]='\\'; dst[o++]='\\'; break;
            case '\n': dst[o++]='\\'; dst[o++]='n'; break;
            case '\r': dst[o++]='\\'; dst[o++]='r'; break;
            case '\t': dst[o++]='\\'; dst[o++]='t'; break;
            case '\b': dst[o++]='\\'; dst[o++]='b'; break;
            case '\f': dst[o++]='\\'; dst[o++]='f'; break;
            default:   dst[o++] = (char)c; break;
        }
    }
    dst[o] = '\0';
    return o;
}

/* Build the NIP-01 serialization [0,pubkey,created_at,kind,tags,content].
   tags_json must be a valid JSON array (e.g. "[]"). Returns length or -1. */
static int nostr_serialize(char *dst, int cap, const char *pubkey_hex,
                           unsigned long created_at, int kind,
                           const char *tags_json, const char *content)
{
    static char esc[SERBUF];
    int n;

    if (nostr_escape(esc, sizeof(esc), content) < 0) return -1;
    n = sprintf(dst, "[0,\"%s\",%lu,%d,%s,\"%s\"]",
                pubkey_hex, created_at, kind, tags_json, esc);
    if (n >= cap) return -1;
    return n;
}

/* Compute the Nostr event id (sha256 of the serialization) as hex. */
static int nostr_event_id(const char *pubkey_hex, unsigned long created_at,
                          int kind, const char *tags_json,
                          const char *content, char out_hex[65])
{
    static char ser[SERBUF];
    int n = nostr_serialize(ser, sizeof(ser), pubkey_hex, created_at, kind,
                            tags_json, content);
    if (n < 0) return 0;
    sha256_hex((const unsigned char *)ser, (unsigned long)n, out_hex);
    return 1;
}

/* ================================================================== */
/* Platform code (real on Amiga, absent in HOST_TEST)                 */
/* ================================================================== */

#ifndef HOST_TEST

/* HOST_NET is a POSIX build of the *same* WebSocket/Nostr code, for running
   the read path on a Linux/Mac host against a local mock relay. It reuses
   every byte of the handshake, framing, parsing and print logic; only the
   socket primitives and the clock differ. It never links AmiSSL (plain ws://
   and --proxy only), which is exactly what the mock-relay demo needs. */
#ifdef HOST_NET
#ifndef NO_AMISSL
#define NO_AMISSL 1
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#define CloseSocket(s) close((int)(s))
/* satisfy the rtc_common.h prototypes (we do not link rtc_common.c here) */
int rtc_net_open(void) { return 1; }
void rtc_net_close(void) { }
int rtc_check_break(void) { return 0; }

#else /* real AmigaOS */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

#ifndef NO_AMISSL
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#endif

extern struct Library *SocketBase;   /* owned by vendored rtc_common.c */
#endif /* HOST_NET */

#ifndef NO_AMISSL
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *UtilityBase = NULL;
static int g_amissl_errno = 0;
static int g_amissl_ready = 0;
#endif

/* -------- config -------- */
static char g_host[256] = "";
static int  g_port = 0;
static char g_path[512] = "/";
static int  g_tls = 0;
static char g_conn_host[256] = "";   /* actual TCP endpoint (proxy override) */
static int  g_conn_port = 0;
static int  g_use_proxy = 0;
static int  g_insecure = 0;
static int  g_verbose = 0;
static long g_limit = 20;
static char g_kinds[128] = "1";
static int  g_eose_exit = 0;

/* -------- receive buffer (raw bytes off the socket) -------- */
static unsigned char g_inbuf[INBUF];
static int g_in_len = 0;
static int g_in_pos = 0;

/* -------- connection abstraction (plain socket or TLS) -------- */
struct ws_conn {
    long sock;
    int  is_tls;
#ifndef NO_AMISSL
    SSL     *ssl;
    SSL_CTX *ctx;
#endif
};

/* bebbo/libnix atoi/strtoul hang on this target; shift-only replacements. */
static unsigned long rc_strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;;) {
        char c = *s; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        if (d >= base) break;
        if (base == 16) v = (v << 4) | (unsigned long)d;
        else v = (v << 3) + (v << 1) + (unsigned long)d;   /* v*10, no helper */
        s++;
    }
    if (end) *end = (char *)s;
    return v;
}

static long rc_atol(const char *s)
{
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    return neg ? -(long)rc_strtoul(s, NULL, 10) : (long)rc_strtoul(s, NULL, 10);
}

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int i;
    char *end;
    for (i = 0; i < 4; i++) {
        parts[i] = rc_strtoul(s, &end, 10);
        if (end == s || parts[i] > 255) return 0;
        if (i < 3 && *end != '.') return 0;
        s = end + 1;
    }
    if (*end != '\0') return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static long tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    unsigned long ip;
    long s;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
#ifdef HOST_NET
    sa.sin_port = htons((unsigned short)port);       /* little-endian host */
#else
    sa.sin_port = (unsigned short)port;              /* m68k big endian: htons is identity */
#endif
    if (parse_ipv4(host, &ip)) {
#ifdef HOST_NET
        sa.sin_addr.s_addr = htonl(ip);
#else
        sa.sin_addr.s_addr = ip;
#endif
    } else {
#ifdef HOST_NET
        struct hostent *he = gethostbyname((char *)host);
#else
        struct hostent *he = gethostbyname((STRPTR)host);
#endif
        if (!he) { fprintf(stderr, "[nostr] cannot resolve %s\n", host); return -1; }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { fprintf(stderr, "[nostr] socket() failed\n"); return -1; }
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "[nostr] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }
    return s;
}

/* -------- AmiSSL setup (follows claude/client/claude.c) -------- */
#ifndef NO_AMISSL
static void amissl_cleanup(void)
{
    if (g_amissl_ready) { CleanupAmiSSLA(NULL); g_amissl_ready = 0; }
    if (AmiSSLBase) { CloseAmiSSL(); AmiSSLBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    if (UtilityBase) { CloseLibrary(UtilityBase); UtilityBase = NULL; }
}

static int amissl_init(void)
{
    if (g_amissl_ready) return 1;
    if (!rtc_net_open()) return 0;
    UtilityBase = OpenLibrary((STRPTR)"utility.library", 0);
    if (!UtilityBase) { fprintf(stderr, "[nostr] cannot open utility.library\n"); return 0; }
    AmiSSLMasterBase = OpenLibrary((STRPTR)"amisslmaster.library", AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        fprintf(stderr, "[nostr] cannot open amisslmaster.library v%d\n"
                        "        install AmiSSL, or use ws:// / --proxy\n",
                        AMISSLMASTER_MIN_VERSION);
        amissl_cleanup();
        return 0;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        fprintf(stderr, "[nostr] AmiSSL version is too old\n");
        amissl_cleanup();
        return 0;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) { fprintf(stderr, "[nostr] cannot open amissl.library\n"); amissl_cleanup(); return 0; }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&g_amissl_errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase, TAG_DONE) != 0) {
        fprintf(stderr, "[nostr] InitAmiSSL failed\n");
        amissl_cleanup();
        return 0;
    }
    g_amissl_ready = 1;
    OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT | OPENSSL_INIT_ADD_ALL_CIPHERS
                     | OPENSSL_INIT_ADD_ALL_DIGESTS, NULL);
    return 1;
}

static void seed_rand(void)
{
    unsigned char buf[64];
    struct DateStamp ds;
    int i;
    DateStamp(&ds);
    for (i = 0; i < (int)sizeof(buf); i++) {
        unsigned long m = (unsigned long)ds.ds_Tick + ds.ds_Minute + i * 2654435761UL;
        buf[i] = (unsigned char)(m ^ (m >> 8) ^ (m >> 16));
    }
    RAND_seed(buf, sizeof(buf));
}
#else
static void amissl_cleanup(void) { }
#endif

/* -------- mask-key RNG (masking need not be crypto-strong) -------- */
static unsigned long g_rng = 0;

static void rng_seed(void)
{
#ifdef HOST_NET
    g_rng = (unsigned long)time(NULL) ^ 0x9E3779B9UL;
#else
    struct DateStamp ds;
    DateStamp(&ds);
    g_rng = (unsigned long)ds.ds_Tick ^ ((unsigned long)ds.ds_Minute << 11)
            ^ ((unsigned long)ds.ds_Days << 17) ^ 0x9E3779B9UL;
#endif
    if (g_rng == 0) g_rng = 0x1234567UL;
}

static unsigned long rng_next(void)
{
    /* xorshift32 - shifts only, no multiply */
    g_rng ^= (g_rng << 13) & M32;
    g_rng ^= (g_rng & M32) >> 17;
    g_rng ^= (g_rng << 5) & M32;
    g_rng &= M32;
    return g_rng;
}

static void gen_mask(unsigned char mk[4])
{
    unsigned long r = rng_next();
    mk[0] = (unsigned char)(r & 0xFF);
    mk[1] = (unsigned char)((r >> 8) & 0xFF);
    mk[2] = (unsigned char)((r >> 16) & 0xFF);
    mk[3] = (unsigned char)((r >> 24) & 0xFF);
}

/* -------- conn read/write -------- */
static long conn_write(struct ws_conn *c, const void *buf, long len)
{
#ifndef NO_AMISSL
    if (c->is_tls) return (long)SSL_write(c->ssl, buf, (int)len);
#endif
    return send((int)c->sock, (void *)buf, len, 0);
}

static long conn_read(struct ws_conn *c, void *buf, long len)
{
#ifndef NO_AMISSL
    if (c->is_tls) return (long)SSL_read(c->ssl, buf, (int)len);
#endif
    return recv((int)c->sock, buf, len, 0);
}

static void conn_close(struct ws_conn *c)
{
#ifndef NO_AMISSL
    if (c->is_tls && c->ssl) { SSL_shutdown(c->ssl); }
#endif
    if (c->sock >= 0) CloseSocket(c->sock);
#ifndef NO_AMISSL
    if (c->ssl) { SSL_free(c->ssl); c->ssl = NULL; }
    if (c->ctx) { SSL_CTX_free(c->ctx); c->ctx = NULL; }
#endif
    c->sock = -1;
}

/* Open the TCP (and TLS if wss) connection to the relay. Returns 1 ok. */
static int conn_open(struct ws_conn *c)
{
    memset(c, 0, sizeof(*c));
    c->sock = -1;
    if (!rtc_net_open()) return 0;

    if (!g_tls) {
        c->sock = tcp_connect(g_conn_host, g_conn_port);
        if (c->sock < 0) return 0;
        c->is_tls = 0;
        return 1;
    }

#ifdef NO_AMISSL
    fprintf(stderr, "[nostr] built without AmiSSL; use a ws:// relay or --proxy\n");
    return 0;
#else
    if (!amissl_init()) return 0;
    seed_rand();
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) { fprintf(stderr, "[nostr] SSL_CTX_new failed\n"); return 0; }
    if (g_insecure) {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    } else {
        SSL_CTX_set_default_verify_paths(c->ctx);
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
    }
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) { fprintf(stderr, "[nostr] SSL_new failed\n"); return 0; }
    c->sock = tcp_connect(g_conn_host, g_conn_port);
    if (c->sock < 0) return 0;
    SSL_set_fd(c->ssl, (int)c->sock);
    SSL_set_tlsext_host_name(c->ssl, g_host);
    if (SSL_connect(c->ssl) <= 0) {
        fprintf(stderr, "[nostr] TLS handshake to %s failed", g_host);
        if (!g_insecure) {
            long vr = SSL_get_verify_result(c->ssl);
            if (vr != X509_V_OK)
                fprintf(stderr, " (cert verify: %s; try --insecure)",
                        X509_verify_cert_error_string(vr));
        }
        fprintf(stderr, "\n");
        return 0;
    }
    c->is_tls = 1;
    return 1;
#endif
}

/* -------- WebSocket handshake -------- */

/* case-insensitive byte compare for header names */
static int ci_startswith(const char *hay, const char *pre)
{
    while (*pre) {
        char a = *hay, b = *pre;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        hay++; pre++;
    }
    return 1;
}

/* find the 4-byte CRLFCRLF end-of-headers in a byte buffer, return index of
   the first byte past it, or -1 */
static int find_header_end(const unsigned char *b, int n)
{
    int i;
    for (i = 0; i + 3 < n; i++)
        if (b[i]=='\r' && b[i+1]=='\n' && b[i+2]=='\r' && b[i+3]=='\n')
            return i + 4;
    return -1;
}

static int ws_handshake(struct ws_conn *c)
{
    static char req[1024];
    static char keyraw[16];
    static char keyb64[32];
    static char expect[64];
    int reqlen, i, sep;
    long got;
    const char *acc;
    char hdrline[128];

    /* random 16-byte Sec-WebSocket-Key */
    rng_seed();
    for (i = 0; i < 16; i++)
        keyraw[i] = (char)(rng_next() & 0xFF);
    base64_encode((const unsigned char *)keyraw, 16, keyb64, sizeof(keyb64));
    ws_accept_key(keyb64, expect, sizeof(expect));

    reqlen = sprintf(req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "User-Agent: nostr-amiga/1.0\r\n"
        "\r\n",
        g_path, g_host, keyb64);
    if (conn_write(c, req, reqlen) != reqlen) {
        fprintf(stderr, "[nostr] handshake send failed\n");
        return 0;
    }
    if (g_verbose)
        printf("[nostr] handshake sent, awaiting 101\n");

    /* read into g_inbuf until we see the end of the HTTP headers. Any bytes
       past the header are the first WebSocket frames; keep them. */
    g_in_len = 0; g_in_pos = 0;
    for (;;) {
        if (g_in_len >= (int)sizeof(g_inbuf) - 1) {
            fprintf(stderr, "[nostr] handshake reply too large\n");
            return 0;
        }
        got = conn_read(c, g_inbuf + g_in_len, sizeof(g_inbuf) - g_in_len);
        if (got <= 0) {
            fprintf(stderr, "[nostr] handshake read failed (relay closed?)\n");
            return 0;
        }
        g_in_len += (int)got;
        sep = find_header_end(g_inbuf, g_in_len);
        if (sep >= 0) break;
    }

    /* the header block is ASCII up to sep; validate status + accept */
    {
        char first[64];
        int n = 0;
        while (n < (int)sizeof(first) - 1 && g_inbuf[n] != '\r' && g_inbuf[n] != '\n')
            { first[n] = (char)g_inbuf[n]; n++; }
        first[n] = '\0';
        if (!strstr(first, " 101")) {
            fprintf(stderr, "[nostr] relay did not accept upgrade: %s\n", first);
            return 0;
        }
    }

    /* scan header lines for Sec-WebSocket-Accept and compare */
    acc = NULL;
    {
        int i2 = 0;
        while (i2 < sep) {
            int j = 0;
            while (i2 < sep && g_inbuf[i2] != '\r' && g_inbuf[i2] != '\n'
                   && j < (int)sizeof(hdrline) - 1)
                hdrline[j++] = (char)g_inbuf[i2++];
            hdrline[j] = '\0';
            while (i2 < sep && (g_inbuf[i2] == '\r' || g_inbuf[i2] == '\n')) i2++;
            if (ci_startswith(hdrline, "sec-websocket-accept:")) {
                char *v = hdrline + strlen("sec-websocket-accept:");
                while (*v == ' ' || *v == '\t') v++;
                acc = v;
                break;
            }
            if (j == 0) break;
        }
    }
    if (!acc || strcmp(acc, expect) != 0) {
        fprintf(stderr, "[nostr] Sec-WebSocket-Accept mismatch (got %s, want %s)\n",
                acc ? acc : "(none)", expect);
        return 0;
    }

    /* leftover bytes past the header are queued WebSocket frame data */
    g_in_pos = sep;
    if (g_verbose)
        printf("[nostr] WebSocket open (%d leftover byte(s) buffered)\n",
               g_in_len - g_in_pos);
    return 1;
}

/* -------- framed send / receive over the open socket -------- */

static int ws_send_text(struct ws_conn *c, const char *text)
{
    static unsigned char frame[SENDBUF];
    unsigned char mask[4];
    int plen = (int)strlen(text), flen;

    if (plen > SENDBUF - 14) {
        fprintf(stderr, "[nostr] outgoing message too large\n");
        return 0;
    }
    gen_mask(mask);
    flen = ws_build_frame(frame, sizeof(frame),
                          (const unsigned char *)text, plen, 0x1, mask);
    if (flen < 0) return 0;
    if (conn_write(c, frame, flen) != flen) {
        fprintf(stderr, "[nostr] frame send failed\n");
        return 0;
    }
    return 1;
}

/* Ensure at least n unconsumed bytes are available at g_inbuf+g_in_pos.
   Returns 1 ok, 0 on EOF/error/frame-too-big. */
static int ws_ensure(struct ws_conn *c, int n)
{
    while (g_in_len - g_in_pos < n) {
        long got;
        if (g_in_pos > 0) {
            memmove(g_inbuf, g_inbuf + g_in_pos, g_in_len - g_in_pos);
            g_in_len -= g_in_pos;
            g_in_pos = 0;
        }
        if (g_in_len >= (int)sizeof(g_inbuf)) return 0;   /* frame too big */
        got = conn_read(c, g_inbuf + g_in_len, sizeof(g_inbuf) - g_in_len);
        if (got <= 0) return 0;
        g_in_len += (int)got;
        if (rtc_check_break()) return 0;
    }
    return 1;
}

/* Read one complete WebSocket data message (fragments joined). Answers ping
   frames and honours close frames. Returns:
     1  a data message is in msg (msglen set); *opcode is the message opcode
     0  connection closed cleanly
    -1  error / interrupted */
static int ws_read_message(struct ws_conn *c, char *msg, int maxmsg,
                           int *msglen, int *opcode_out)
{
    static unsigned char pay[PAYBUF];
    int total = 0, first_opcode = -1;

    for (;;) {
        int fin, opcode, plen, consumed;

        /* try to parse a frame from what we have; pull more bytes as needed */
        for (;;) {
            consumed = ws_parse_frame(g_inbuf + g_in_pos, g_in_len - g_in_pos,
                                      pay, sizeof(pay), &fin, &opcode, &plen);
            if (consumed > 0) break;
            if (consumed < 0) { fprintf(stderr, "[nostr] bad frame\n"); return -1; }
            /* need more bytes: ensure at least current + 1 */
            if (!ws_ensure(c, (g_in_len - g_in_pos) + 1)) {
                if (rtc_check_break()) return -1;
                return 0;   /* clean EOF */
            }
        }
        g_in_pos += consumed;

        if (opcode == 0x8) {            /* close */
            if (g_verbose) printf("[nostr] relay sent close\n");
            return 0;
        }
        if (opcode == 0x9) {            /* ping -> pong */
            static unsigned char pong[PAYBUF + 16];
            unsigned char mask[4];
            int fl;
            gen_mask(mask);
            fl = ws_build_frame(pong, sizeof(pong), pay, plen, 0xA, mask);
            if (fl > 0) conn_write(c, pong, fl);
            continue;
        }
        if (opcode == 0xA) continue;    /* pong: ignore */

        /* data frame (text=1, binary=2) or continuation=0 */
        if (first_opcode < 0)
            first_opcode = (opcode == 0) ? 1 : opcode;
        if (total + plen < maxmsg - 1) {
            memcpy(msg + total, pay, plen);
            total += plen;
        } else {
            fprintf(stderr, "[nostr] message exceeds buffer, truncating\n");
        }
        if (fin) {
            msg[total] = '\0';
            *msglen = total;
            *opcode_out = first_opcode;
            return 1;
        }
    }
}

/* -------- print one Nostr EVENT message -------- */

static void print_event(const char *msg)
{
    static char obj[OBJBUF];
    static char content[OBJBUF];
    char pubkey[80];
    char created[32];
    char kind[16];

    if (!extract_first_object(msg, obj, sizeof(obj))) {
        fprintf(stderr, "[nostr] EVENT without an object\n");
        return;
    }
    pubkey[0] = content[0] = created[0] = kind[0] = '\0';
    get_json_string(obj, "pubkey", pubkey, sizeof(pubkey));
    get_json_string(obj, "content", content, sizeof(content));
    get_json_string(obj, "created_at", created, sizeof(created)); /* number: raw scan below */
    if (created[0] == '\0') {
        /* created_at is a JSON number, not a string; pull it raw */
        const char *p = find_key(obj, "created_at");
        if (p) {
            int o = 0;
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p >= '0' && *p <= '9' && o < (int)sizeof(created) - 1)
                created[o++] = *p++;
            created[o] = '\0';
        }
    }
    {
        const char *p = find_key(obj, "kind");
        if (p) {
            int o = 0;
            while (*p == ' ' || *p == '\t') p++;
            while (*p && *p >= '0' && *p <= '9' && o < (int)sizeof(kind) - 1)
                kind[o++] = *p++;
            kind[o] = '\0';
        }
    }

    printf("----------------------------------------\n");
    printf("author : %s\n", pubkey);
    if (created[0]) printf("at     : %s (kind %s)\n", created, kind[0]?kind:"?");
    printf("%s\n", content);
    fflush(stdout);
}

/* -------- the read-only subscribe loop -------- */

static int run_subscribe(void)
{
    struct ws_conn c;
    static char req[256];
    static char msg[MSGBUF];
    int msglen, opcode, nevents = 0;

    if (!conn_open(&c)) return 0;
    if (g_verbose)
        printf("[nostr] connected to %s:%d (%s)\n", g_conn_host, g_conn_port,
               g_tls ? "TLS" : "plain");
    if (!ws_handshake(&c)) { conn_close(&c); return 0; }

    sprintf(req, "[\"REQ\",\"sub1\",{\"kinds\":[%s],\"limit\":%ld}]",
            g_kinds, g_limit);
    if (g_verbose) printf("[nostr] -> %s\n", req);
    if (!ws_send_text(&c, req)) { conn_close(&c); return 0; }

    for (;;) {
        int r = ws_read_message(&c, msg, sizeof(msg), &msglen, &opcode);
        if (r == 0) { printf("[nostr] connection closed\n"); break; }
        if (r < 0) { printf("[nostr] stopped\n"); break; }
        if (opcode != 0x1) continue;   /* only text frames carry Nostr JSON */

        {
            char tag[24];
            if (!first_array_string(msg, tag, sizeof(tag))) {
                if (g_verbose) printf("[nostr] (unparsed) %s\n", msg);
                continue;
            }
            if (strcmp(tag, "EVENT") == 0) {
                print_event(msg);
                nevents++;
            } else if (strcmp(tag, "EOSE") == 0) {
                printf("---- end of stored events (%d shown) ----\n", nevents);
                fflush(stdout);
                if (g_eose_exit) break;
            } else if (strcmp(tag, "NOTICE") == 0) {
                printf("[relay notice] %s\n", msg);
                fflush(stdout);
            } else if (strcmp(tag, "CLOSED") == 0) {
                printf("[relay closed subscription] %s\n", msg);
                fflush(stdout);
                break;
            } else if (g_verbose) {
                printf("[nostr] %s: %s\n", tag, msg);
            }
        }
        if (rtc_check_break()) { printf("\n[nostr] interrupted\n"); break; }
    }

    /* politely close the subscription and socket */
    ws_send_text(&c, "[\"CLOSE\",\"sub1\"]");
    conn_close(&c);
    return 1;
}

/* -------- --publish (serialize + id only; signing is a TODO) -------- */

static void do_publish_demo(const char *content, const char *pubkey)
{
    static char ser[SERBUF];
    char idhex[65];
    unsigned long created;
    const char *pk = (pubkey && pubkey[0]) ? pubkey
        : "0000000000000000000000000000000000000000000000000000000000000000";

#ifdef HOST_NET
    created = (unsigned long)time(NULL);
#else
    {
        /* created_at from the Amiga clock (seconds since 1970). DateStamp
           gives days since 1978-01-01, which is 2922 days after the 1970
           epoch. The *60 / *86400 here are constant multiplies (strength
           reduced by the compiler), not the runtime-variable ones that
           trip bebbo's __mulsi3. */
        struct DateStamp ds;
        DateStamp(&ds);
        created = ((unsigned long)ds.ds_Days + 2922UL) * 86400UL
                + (unsigned long)ds.ds_Minute * 60UL
                + (unsigned long)ds.ds_Tick / 50UL;
    }
#endif

    nostr_serialize(ser, sizeof(ser), pk, created, 1, "[]", content);
    nostr_event_id(pk, created, 1, "[]", content, idhex);

    printf("Nostr event (kind 1) built. NOT SIGNED, so NOT published.\n\n");
    printf("serialization (NIP-01):\n%s\n\n", ser);
    printf("event id (sha256)      : %s\n", idhex);
    printf("pubkey (x-only, hex)   : %s\n\n", pk);
    printf("TODO: to publish, sign the event id above with a secp256k1\n");
    printf("      Schnorr signature (BIP340), then send\n");
    printf("      [\"EVENT\",{...,\"id\":\"<id>\",\"sig\":\"<64-byte hex>\"}].\n");
    printf("      secp256k1 is not vendored here; see README.md.\n");
    fflush(stdout);
}

/* -------- URL + argument parsing -------- */

static int parse_ws_url(const char *url)
{
    const char *p, *slash, *colon;
    int hlen;

    if (strncmp(url, "wss://", 6) == 0) { g_tls = 1; p = url + 6; g_port = 443; }
    else if (strncmp(url, "ws://", 5) == 0) { g_tls = 0; p = url + 5; g_port = 80; }
    else { fprintf(stderr, "[nostr] URL must start with ws:// or wss://\n"); return 0; }

    slash = strchr(p, '/');
    colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;
    hlen = (int)((colon ? colon : (slash ? slash : p + strlen(p))) - p);
    if (hlen <= 0 || hlen >= (int)sizeof(g_host)) return 0;
    memcpy(g_host, p, hlen);
    g_host[hlen] = '\0';
    if (colon) {
        g_port = (int)rc_atol(colon + 1);
        if (g_port <= 0 || g_port > 65535) return 0;
    }
    if (slash) {
        strncpy(g_path, slash, sizeof(g_path) - 1);
        g_path[sizeof(g_path) - 1] = '\0';
    } else {
        strcpy(g_path, "/");
    }
    return 1;
}

static void usage(void)
{
    printf("nostr - a native Nostr client for AmigaOS (read-only)\n\n");
    printf("Usage:\n");
    printf("  nostr [options] <relay-url>\n");
    printf("  nostr --publish \"text\" [--pubkey HEX]   (build+id only; no signing)\n\n");
    printf("Relay URL:  ws://host[:port][/path]  or  wss://host[:port][/path]\n\n");
    printf("Options:\n");
    printf("  --limit N        number of stored events to request (default 20)\n");
    printf("  --kinds a,b,c    event kinds to subscribe to (default 1 = notes)\n");
    printf("  --eose-exit      stop after the relay signals end-of-stored-events\n");
    printf("  --proxy host:port  TCP to this endpoint (plaintext) instead of the\n");
    printf("                     URL host; lets a host proxy terminate TLS\n");
    printf("  --insecure       skip TLS certificate verification (wss only)\n");
    printf("  -v, --verbose    print transport progress\n");
    printf("  -h, --help       this help\n");
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    const char *pub_text = NULL;
    const char *pub_key = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            g_limit = rc_atol(argv[++i]);
        } else if (strcmp(argv[i], "--kinds") == 0 && i + 1 < argc) {
            strncpy(g_kinds, argv[++i], sizeof(g_kinds) - 1);
            g_kinds[sizeof(g_kinds) - 1] = '\0';
        } else if (strcmp(argv[i], "--eose-exit") == 0) {
            g_eose_exit = 1;
        } else if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            char hp[160], *colon;
            strncpy(hp, argv[++i], sizeof(hp) - 1);
            hp[sizeof(hp) - 1] = '\0';
            colon = strchr(hp, ':');
            if (colon) { *colon = '\0'; g_conn_port = (int)rc_atol(colon + 1); }
            strncpy(g_conn_host, hp, sizeof(g_conn_host) - 1);
            g_conn_host[sizeof(g_conn_host) - 1] = '\0';
            g_use_proxy = 1;
        } else if (strcmp(argv[i], "--insecure") == 0) {
            g_insecure = 1;
        } else if (strcmp(argv[i], "--publish") == 0 && i + 1 < argc) {
            pub_text = argv[++i];
        } else if (strcmp(argv[i], "--pubkey") == 0 && i + 1 < argc) {
            pub_key = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] != '-') {
            url = argv[i];
        } else {
            fprintf(stderr, "[nostr] unknown option %s\n", argv[i]);
            usage();
            return 20;
        }
    }

    if (pub_text) {
        do_publish_demo(pub_text, pub_key);
        return 0;
    }

    if (!url) { usage(); return 5; }
    if (!parse_ws_url(url)) return 20;

    /* the TCP endpoint is the URL host unless --proxy overrode it */
    if (!g_use_proxy) {
        strcpy(g_conn_host, g_host);
        g_conn_port = g_port;
    }

    {
        int ok = run_subscribe();
        amissl_cleanup();
        rtc_net_close();
        return ok ? 0 : 20;
    }
}

#else /* ================= HOST_TEST: pure-function self-test ============= */

static int failures = 0;

static void check(const char *what, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) failures++;
}

int main(void)
{
    printf("nostr.c host self-test\n");
    printf("======================\n");

    /* 1. sha256 known vectors */
    {
        char h[65];
        sha256_hex((const unsigned char *)"", 0, h);
        check("sha256(\"\")",
              strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
        sha256_hex((const unsigned char *)"abc", 3, h);
        check("sha256(\"abc\")",
              strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    }

    /* 2. base64 vectors */
    {
        char o[32];
        base64_encode((const unsigned char *)"", 0, o, sizeof(o));
        check("b64(\"\")", strcmp(o, "") == 0);
        base64_encode((const unsigned char *)"f", 1, o, sizeof(o));
        check("b64(\"f\")", strcmp(o, "Zg==") == 0);
        base64_encode((const unsigned char *)"fo", 2, o, sizeof(o));
        check("b64(\"fo\")", strcmp(o, "Zm8=") == 0);
        base64_encode((const unsigned char *)"foo", 3, o, sizeof(o));
        check("b64(\"foo\")", strcmp(o, "Zm9v") == 0);
        base64_encode((const unsigned char *)"foobar", 6, o, sizeof(o));
        check("b64(\"foobar\")", strcmp(o, "Zm9vYmFy") == 0);
    }

    /* 3. sha1 + base64 = the RFC6455 accept-key example */
    {
        char acc[64];
        ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof(acc));
        check("RFC6455 Sec-WebSocket-Accept",
              strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
    }

    /* 4. WebSocket frame encode -> decode round trip (masked client frame) */
    {
        static unsigned char frame[256];
        static unsigned char pay[256];
        unsigned char mask[4];
        const char *text = "[\"REQ\",\"sub1\",{\"kinds\":[1],\"limit\":20}]";
        int plen = (int)strlen(text), flen, fin, op, dlen, consumed, i, allmasked = 1;

        mask[0]=0x12; mask[1]=0x34; mask[2]=0x56; mask[3]=0x78;
        flen = ws_build_frame(frame, sizeof(frame),
                              (const unsigned char *)text, plen, 0x1, mask);
        check("frame builds", flen > 0);
        check("frame has FIN|text opcode", frame[0] == 0x81);
        check("frame marks MASK bit", (frame[1] & 0x80) != 0);
        /* the payload bytes must not appear in the clear (masked) */
        for (i = 0; i < plen; i++)
            if (frame[flen - plen + i] == (unsigned char)text[i]) { allmasked = 0; }
        check("payload is masked on the wire", allmasked || plen == 0);

        consumed = ws_parse_frame(frame, flen, pay, sizeof(pay), &fin, &op, &dlen);
        check("frame parses fully", consumed == flen);
        check("decoded FIN set", fin == 1);
        check("decoded opcode text", op == 0x1);
        check("decoded length matches", dlen == plen);
        pay[dlen] = '\0';
        check("decoded payload round-trips", strcmp((char *)pay, text) == 0);
    }

    /* 5. frame decode of a server (unmasked) frame + partial-read handling */
    {
        unsigned char sf[8];
        unsigned char pay[16];
        int fin, op, dlen, consumed;

        /* server text frame "hi": FIN|text, len 2, no mask */
        sf[0] = 0x81; sf[1] = 0x02; sf[2] = 'h'; sf[3] = 'i';
        consumed = ws_parse_frame(sf, 4, pay, sizeof(pay), &fin, &op, &dlen);
        check("server frame parses", consumed == 4 && dlen == 2);
        pay[dlen] = '\0';
        check("server payload correct", strcmp((char *)pay, "hi") == 0);
        /* only 3 of 4 bytes present -> incomplete */
        consumed = ws_parse_frame(sf, 3, pay, sizeof(pay), &fin, &op, &dlen);
        check("short frame reports incomplete", consumed == 0);
    }

    /* 6. extended 126-length frame decode */
    {
        static unsigned char big[400];
        static unsigned char pay[400];
        int fin, op, dlen, consumed, i;
        big[0] = 0x82;            /* FIN|binary */
        big[1] = 126;             /* extended 16-bit length */
        big[2] = 0x01; big[3] = 0x2C;   /* 300 */
        for (i = 0; i < 300; i++) big[4 + i] = (unsigned char)(i & 0xFF);
        consumed = ws_parse_frame(big, 304, pay, sizeof(pay), &fin, &op, &dlen);
        check("126-length frame parses", consumed == 304 && dlen == 300);
        check("126-length opcode binary", op == 0x2);
    }

    /* 7. Nostr JSON parsing of an EVENT message */
    {
        const char *msg =
            "[\"EVENT\",\"sub1\",{\"id\":\"abc\",\"pubkey\":\"deadbeef\","
            "\"created_at\":1700000000,\"kind\":1,\"tags\":[],"
            "\"content\":\"hello \\\"world\\\"\\nline2\",\"sig\":\"ff\"}]";
        char tag[24], obj[512], pk[80], content[128];
        check("first array string is EVENT",
              first_array_string(msg, tag, sizeof(tag)) && strcmp(tag, "EVENT") == 0);
        check("event object extracted", extract_first_object(msg, obj, sizeof(obj)));
        get_json_string(obj, "pubkey", pk, sizeof(pk));
        check("pubkey parsed", strcmp(pk, "deadbeef") == 0);
        get_json_string(obj, "content", content, sizeof(content));
        check("content parsed + unescaped",
              strcmp(content, "hello \"world\"\nline2") == 0);
    }

    /* 8. EOSE detection */
    {
        char tag[24];
        first_array_string("[\"EOSE\",\"sub1\"]", tag, sizeof(tag));
        check("EOSE tag parsed", strcmp(tag, "EOSE") == 0);
    }

    /* 9. Nostr event serialization + id vs an independent (python) vector */
    {
        static char ser[256];
        char idhex[65];
        const char *pk = "0000000000000000000000000000000000000000000000000000000000000001";
        int n = nostr_serialize(ser, sizeof(ser), pk, 1700000000UL, 1, "[]", "hello");
        check("serialization exact",
              n > 0 && strcmp(ser,
              "[0,\"0000000000000000000000000000000000000000000000000000000000000001\",1700000000,1,[],\"hello\"]") == 0);
        nostr_event_id(pk, 1700000000UL, 1, "[]", "hello", idhex);
        check("event id matches python sha256",
              strcmp(idhex, "b8591d69d0638d47eb20e0505fdbaf565e52675fa998010df62813ad3d11b486") == 0);
    }

    /* 10. Nostr event id with escaped content (2nd vector) */
    {
        char idhex[65];
        const char *pk = "0000000000000000000000000000000000000000000000000000000000000001";
        nostr_event_id(pk, 1700000000UL, 1, "[]",
                       "he said \"hi\"\nbye\tx", idhex);
        check("event id (escaped content) matches python",
              strcmp(idhex, "621824ef5f2b5e74c6e261973112d56b42c238308b0e197c47ab378d372bac15") == 0);
    }

    printf("======================\n");
    if (failures == 0) printf("ALL CHECKS PASSED (0 failures)\n");
    else printf("%d CHECK(S) FAILED\n", failures);
    return failures ? 1 : 0;
}

#endif /* HOST_TEST */
