#!/bin/sh
# Build MicroPython for classic m68k AmigaOS (RustChain Amiga Edition, Phase 3).
#
# Base: OoZe1911/micropython-amiga-port (MicroPython v1.28 based, 68020+),
# vendored at src/micropython-amiga-port/ — see README.md for provenance.
# Toolchain: amigadev/crosstools:m68k-amigaos docker image (bebbo gcc 6.5.0b),
# same image the miner and SDK use. The upstream port expects the toolchain at
# /opt/amiga/bin/ and the AmiSSL SDK preinstalled; this script adapts both
# without modifying the vendored source:
#   - mpy-cross (a BUILD-HOST tool, needed for the frozen-module manifest) is
#     built on this host first. The docker image has NO native compiler (its
#     cc/gcc are aliases of the m68k cross gcc), and passing CROSS_COMPILE on
#     the make command line poisons the mpy-cross sub-make. The host binary
#     was verified to run inside the container.
#   - the container gets a /opt/amiga -> /opt/m68k-amigaos symlink so the
#     vendored Makefile's hardcoded CROSS_COMPILE path just works.
#   - AmiSSL SDK 5.27 (src/amissl-sdk/, from jens-maus/AmiSSL releases, open
#     source) is copied into the ephemeral container's ndk-include + libnix
#     lib dirs before make runs.
#
# Output: bin/upython (AmigaOS hunk executable).
set -e
cd "$(dirname "$0")"

PORTDIR=src/micropython-amiga-port
SDKDIR=$PWD/src/amissl-sdk/AmiSSL/Developer
IMAGE=amigadev/crosstools:m68k-amigaos

if [ ! -d "$PORTDIR" ]; then
    echo "missing $PORTDIR — clone https://github.com/OoZe1911/micropython-amiga-port there" >&2
    exit 1
fi
if [ ! -d "$SDKDIR" ]; then
    echo "missing AmiSSL SDK — download AmiSSL-5.27-SDK.lha from" >&2
    echo "https://github.com/jens-maus/amissl/releases and extract under src/amissl-sdk/" >&2
    exit 1
fi

# 1. Host tool: mpy-cross (skip if already built)
if [ ! -x "$PORTDIR/mpy-cross/build/mpy-cross" ]; then
    make -C "$PORTDIR/mpy-cross" -j8
fi

# 2. Cross build in the container
docker run --rm \
    -v "$PWD/$PORTDIR:/work" \
    -v "$SDKDIR:/amissl:ro" \
    -w /work/ports/amiga \
    "$IMAGE" sh -c "
        set -e
        ln -sfn /opt/m68k-amigaos /opt/amiga
        TC=/opt/m68k-amigaos/m68k-amigaos
        # AmiSSL headers (proto/, libraries/, amissl/, openssl/, inline/...)
        cp -r /amissl/include/. \$TC/ndk-include/
        # AmiSSL link stubs (AmigaOS3 build for bebbo gcc)
        cp /amissl/lib/AmigaOS3/libamisslstubs.a \$TC/libnix/lib/
        cp /amissl/lib/AmigaOS3/libamisslstubs.a \$TC/libnix/lib/libm020/
        make -j8
        chown -R $(id -u):$(id -g) /work
    "

mkdir -p bin
cp "$PORTDIR/ports/amiga/build/micropython" bin/upython
file bin/upython
echo "OK: bin/upython"
