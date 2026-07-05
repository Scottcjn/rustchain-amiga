#!/bin/sh
# Build the MINIMAL MicroPython for m68k AmigaOS (bare-ROM fallback).
# Source: vanilla micropython v1.25.0 (src/micropython/) + our from-scratch
# port overlay (port/amiga/). Depends only on ROM libraries (exec/dos), so it
# runs from a bare AROS-ROM FS-UAE boot with no Libs/ directory.
# Output: bin/upython-min (AmigaOS hunk executable).
set -e
cd "$(dirname "$0")"

TREE=src/micropython
IMAGE=amigadev/crosstools:m68k-amigaos

if [ ! -d "$TREE" ]; then
    echo "missing $TREE — clone micropython at tag v1.25.0 there" >&2
    exit 1
fi

# Overlay our port into the vanilla tree (rsync so edits propagate)
mkdir -p "$TREE/ports/amiga"
cp -f port/amiga/Makefile port/amiga/main.c port/amiga/mpconfigport.h \
      port/amiga/mphalport.h port/amiga/qstrdefsport.h "$TREE/ports/amiga/"

docker run --rm \
    -v "$PWD/$TREE:/work" \
    -w /work/ports/amiga \
    "$IMAGE" sh -c "make -j8 \$*; chown -R $(id -u):$(id -g) /work/ports/amiga"

mkdir -p bin
cp "$TREE/ports/amiga/build/upython-min" bin/upython-min
file bin/upython-min
echo "OK: bin/upython-min"
