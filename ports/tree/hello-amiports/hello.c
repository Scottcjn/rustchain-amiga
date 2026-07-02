/*
 * hello.c - proof-of-build program for the amiports system.
 *
 * This file is cross-compiled by ports/harness/amiport-build.py using
 * the amigadev/crosstools:m68k-amigaos docker toolchain, packaged as an
 * .apak, served over HTTP, and installed INSIDE the emulated Amiga by
 * the amiport client. When you see its output in the test log, the
 * whole source-to-guest pipeline worked.
 *
 * C89, stdio only, runs on any AmigaOS 2.0+/AROS m68k.
 */

#include <stdio.h>

int main(void)
{
    printf("hello from amiports!\n");
    printf("this binary was built from source by the amiports harness,\n");
    printf("fetched over HTTP, sha1-verified and installed by amiport.\n");
    printf("HELLO_AMIPORTS_PROOF_V1\n");
    return 0;
}
