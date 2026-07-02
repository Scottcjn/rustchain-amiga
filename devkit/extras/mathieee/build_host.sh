#!/bin/sh
# Build the mathieee replacement libraries on the host, using the devkit's
# own NATIVE Amiga vasm/vlink executed under vamos (amitools m68k runtime).
# Needs: python venv with amitools + machine68k 0.3.0 (see devkit/README.md).
#
# Usage: ./build_host.sh [path-to-vamos]
set -e
cd "$(dirname "$0")"
VAMOS="${1:-vamos}"
DEV=/home/scott/rustchain-amiga/devkit/Development
RUN="$VAMOS -C 68020 -m 10240 -s 256 -V DH1:$DEV -V SYS:."

mkdir -p build
for lib in mathieeedoubbas mathieeedoubtrans; do
    $RUN -- DH1:vbcc/bin/vasmm68k_mot -quiet -Fhunk -m68020 -m68881 \
        SYS:$lib.asm -o SYS:build/$lib.o
    $RUN -- DH1:vbcc/bin/vlink -bamigahunk -x -nostdlib \
        SYS:build/$lib.o -o SYS:build/$lib.library
    file build/$lib.library
done
echo OK
