#!/usr/bin/env bash
# build.sh - cross-compile Lua 5.4.7 for m68k AmigaOS, plus a host build
# of the exact same (patched) sources as a correctness sanity check.
#
# Usage:
#   ./build.sh            build everything (cross + host)
#   ./build.sh cross      build only the AmigaOS cross binaries
#   ./build.sh host       build only the host sanity binary
#
# Requires: docker (amigadev/crosstools:m68k-amigaos image), a host cc.
# Safe to re-run: wipes and repopulates build/amiga and build/host each time.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

LUA_VERSION=5.4.7
LUA_SRC_DIR="src-lua/lua-${LUA_VERSION}/src"
PATCHED_LUACONF="patches/luaconf-amiga.h"
LIBM_COMPAT="patches/lua_libm_compat_amiga.c"

if [ ! -d "$LUA_SRC_DIR" ]; then
  echo "build.sh: $LUA_SRC_DIR not found." >&2
  echo "  Fetch it first, e.g.:" >&2
  echo "    cd src-lua && curl -sSLO https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz && tar xzf lua-${LUA_VERSION}.tar.gz" >&2
  exit 1
fi

# Everything except lua.c (the interpreter's main()) and luac.c (the
# compiler's main()) - the shared Lua core + standard libraries.
CORE_SRCS="lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio"

DOCKER_IMAGE="amigadev/crosstools:m68k-amigaos"
CROSS_CC="m68k-amigaos-gcc"
# -m68020, not -m68000: see README.md ("why -m68020"). -noixemul selects
# the libnix C library (not ixemul, not clib2) - matches the rest of this
# repo's cross-compiled ports.
CROSS_CFLAGS="-noixemul -m68020 -O2 -fomit-frame-pointer -Wall -Wextra"

stage_sources() {
  # $1 = target dir. Copies pristine Lua sources + drops in our patched
  # luaconf.h + the log2f compat shim (see patches/lua_libm_compat_amiga.c).
  local dst="$1"
  rm -rf "$dst"
  mkdir -p "$dst"
  cp "$LUA_SRC_DIR"/*.c "$LUA_SRC_DIR"/*.h "$dst"/
  cp "$PATCHED_LUACONF" "$dst/luaconf.h"
  cp "$LIBM_COMPAT" "$dst"/
}

build_cross() {
  echo "=== cross-compiling for m68k AmigaOS (docker: $DOCKER_IMAGE) ==="
  local dst="build/amiga"
  stage_sources "$dst"

  echo "-- compiling: interpreter + compiler core (${CORE_SRCS// /, })"
  # shellcheck disable=SC2086
  docker run --rm -v "$HERE/$dst:/work" -w /work "$DOCKER_IMAGE" bash -c '
    set -e
    for f in '"$CORE_SRCS"' lua_libm_compat_amiga lua luac; do
      echo "  cc  $f.c"
      '"$CROSS_CC"' '"$CROSS_CFLAGS"' -c "$f.c" -o "$f.o"
    done
  '

  echo "-- linking bin/lua"
  # shellcheck disable=SC2086
  docker run --rm -v "$HERE/$dst:/work" -w /work "$DOCKER_IMAGE" bash -c '
    set -e
    OBJS=""
    for f in '"$CORE_SRCS"' lua_libm_compat_amiga; do OBJS="$OBJS $f.o"; done
    '"$CROSS_CC"' '"$CROSS_CFLAGS"' -o lua $OBJS lua.o -lm
  '
  echo "-- linking bin/luac"
  # shellcheck disable=SC2086
  docker run --rm -v "$HERE/$dst:/work" -w /work "$DOCKER_IMAGE" bash -c '
    set -e
    OBJS=""
    for f in '"$CORE_SRCS"' lua_libm_compat_amiga; do OBJS="$OBJS $f.o"; done
    '"$CROSS_CC"' '"$CROSS_CFLAGS"' -o luac $OBJS luac.o -lm
  '
  # docker leaves root-owned outputs; fix up so the host can copy them
  docker run --rm -v "$HERE/$dst:/work" -w /work "$DOCKER_IMAGE" \
    chmod 0666 lua luac || true

  mkdir -p bin
  cp "$dst/lua" bin/lua
  cp "$dst/luac" bin/luac
  echo "-- verifying with file(1):"
  file bin/lua bin/luac
  case "$(file -b bin/lua)" in
    *"AmigaOS loadseg()ble"*) echo "   OK: bin/lua is an AmigaOS hunk binary" ;;
    *) echo "   FAIL: bin/lua does not look like an AmigaOS binary" >&2; exit 1 ;;
  esac
}

build_host() {
  echo "=== host sanity build (same sources, same luaconf.h, native cc) ==="
  local dst="build/host"
  stage_sources "$dst"
  (
    cd "$dst"
    SRC_C=""
    for f in $CORE_SRCS lua; do SRC_C="$SRC_C $f.c"; done
    # shellcheck disable=SC2086
    ${CC:-cc} -O2 -Wall -c $SRC_C
    OBJS=""
    for f in $CORE_SRCS; do OBJS="$OBJS $f.o"; done
    # shellcheck disable=SC2086
    ${CC:-cc} -O2 -o lua $OBJS lua.o -lm
  )
  mkdir -p host
  cp "$dst/lua" host/lua
  echo "-- running host/verify.lua:"
  host/lua host/verify.lua | tee host/verify_output.txt
  if ! grep -q "ALL TESTS COMPLETED OK" host/verify_output.txt; then
    echo "FAIL: host verify script did not complete" >&2
    exit 1
  fi
  echo "   OK: host lua runs correctly (print(6*7)=42, loops, strings, etc)"
}

case "${1:-all}" in
  cross) build_cross ;;
  host)  build_host ;;
  all)   build_cross; build_host ;;
  *) echo "usage: $0 [cross|host|all]" >&2; exit 2 ;;
esac
