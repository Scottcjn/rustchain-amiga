/*
 * rc_u64.c - 64-bit helpers safe for m68k / bebbo gcc / libnix.
 * Extracted unchanged from the working miner (rustchain_amiga_miner.c).
 */

#include "rustchain/rc_u64.h"

/* unsigned 64-bit to decimal, printf %lld is not a safe bet in libnix */
char *rc_u64s(unsigned long long v, char *buf)
{
    char tmp[24];
    int i = 0, o = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (v > 0) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i > 0) buf[o++] = tmp[--i];
    buf[o] = '\0';
    return buf;
}

/*
 * Shift-free integer square root. The first version used 64-bit shifts
 * (1ULL<<62, r>>1) and bebbo gcc at -m68000 miscompiled them: on target
 * stdev came back exactly 2^34 while the x86 host test passed. The live
 * run proved 64-bit add, sub, compare and divide all work on target
 * (variance and its printed decimal were correct), so binary search on
 * those ops only. mid <= x/mid avoids the mid*mid overflow. ~32 divides.
 */
unsigned long long rc_isqrt64(unsigned long long x)
{
    unsigned long long lo = 1, hi = 4294967295ULL, mid;

    if (x == 0) return 0;
    if (x < hi) hi = x;
    while (lo < hi) {
        mid = lo + (hi - lo + 1) / 2;
        if (mid <= x / mid) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
