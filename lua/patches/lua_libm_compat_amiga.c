/*
** lua_libm_compat_amiga.c
**
** AmigaOS m68k / libnix compat shim, NOT part of upstream Lua.
**
** libnix's libm020/libm.a on this toolchain (amigadev/crosstools:m68k-amigaos,
** gcc 6.5.0b) ships log2() (double) but its log2f() (float) object file
** (sf_log2.o) is empty - the symbol is simply missing from the archive.
** Every other single-precision math function lmathlib.c needs (acosf,
** asinf, atan2f, ceilf, cosf, expf, fabsf, floorf, fmodf, logf, log10f,
** powf, sinf, sqrtf, tanf) IS present in libm020/libm.a. log2f is the
** one gap, only reached by math.log(x, 2) - a rare call.
**
** Fix: define float log2f() ourselves in terms of the double log2()
** that libnix does provide. This is the only non-Lua source file
** added by the AmigaOS port; it is compiled and linked only into the
** cross (m68k) build, not the host sanity build (host libm has a
** real log2f already).
*/

#include <math.h>

float log2f(float x)
{
    return (float)log2((double)x);
}
