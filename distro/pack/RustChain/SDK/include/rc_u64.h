/*
 * rc_u64.h - 64-bit integer helpers that are safe on m68k with bebbo gcc.
 *
 * Two hard lessons from the live FS-UAE miner run are baked in here:
 *
 * 1. Do NOT use printf("%lld") on libnix. It is not a safe bet; print
 *    64-bit values with rc_u64s() instead.
 * 2. Do NOT use 64-bit shifts at -m68000. bebbo gcc 6.5 miscompiled
 *    (1ULL<<62) and (r>>1); rc_isqrt64() is shift-free by design and
 *    uses only 64-bit add/sub/compare/divide, the ops the live run
 *    proved good on target.
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_U64_H
#define RC_U64_H

/*
 * Unsigned 64-bit to decimal string. buf must hold at least 21 bytes
 * (20 digits for u64 max plus the NUL). Returns buf.
 */
char *rc_u64s(unsigned long long v, char *buf);

/*
 * Shift-free integer square root, floor(sqrt(x)).
 * Binary search with mid <= x/mid to avoid mid*mid overflow. About 32
 * divides per call. See the header comment for why no shifts.
 */
unsigned long long rc_isqrt64(unsigned long long x);

#endif /* RC_U64_H */
