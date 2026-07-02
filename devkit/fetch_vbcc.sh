#!/bin/sh
# Fetch the devkit's third-party pieces from their official sources and
# (re)build the Development drawer layout. This is the "user fetches it"
# path for anything the public distro cannot legally bake into its image.
#
# Downloads:
#   vbcc 0.9h p3 hosted Amiga binaries + m68k-amigaos target (vbcc.de/hasenbraten)
#   NDK 3.2 R4 (Aminet, uploaded by Hyperion Entertainment)
#
# Needs: curl, lha (lhasa). Run from anywhere.
set -e
BASE="$(cd "$(dirname "$0")" && pwd)"
DL="$BASE/downloads"
DEV="$BASE/Development"
mkdir -p "$DL"
cd "$DL"

get() { # url sha1
    f="$(basename "$1")"
    [ -f "$f" ] || curl -sfLO "$1"
    echo "$2  $f" | sha1sum -c - || { echo "SHA-1 MISMATCH for $f"; exit 1; }
}

get http://phoenix.owl.de/vbcc/2022-03-23/vbcc_bin_amigaos68k.lha \
    2e245649b55217a224d8b135877850dd3150425d
get http://phoenix.owl.de/vbcc/2022-05-22/vbcc_target_m68k-amigaos.lha \
    c3ed907c9d9535508ea95c597fb0ecd6987412d6
get http://aminet.net/dev/misc/NDK3.2.lha \
    94c730f79c89febd2f337ece60f656c8499c104e

rm -rf x-bin x-target x-ndk
mkdir -p x-bin x-target x-ndk
(cd x-bin    && lha xq ../vbcc_bin_amigaos68k.lha)
(cd x-target && lha xq ../vbcc_target_m68k-amigaos.lha)
(cd x-ndk    && lha xq ../NDK3.2.lha)

mkdir -p "$DEV/vbcc"
cp -r x-bin/vbcc_bin_amigaos68k/bin        "$DEV/vbcc/"
cp -r x-bin/vbcc_bin_amigaos68k/doc        "$DEV/vbcc/"
cp    x-bin/vbcc_bin_amigaos68k/vbcc_version "$DEV/vbcc/"
mkdir -p "$DEV/vbcc/config"
cp    x-bin/vbcc_bin_amigaos68k/config/vc.config "$DEV/vbcc/config/" 2>/dev/null || true
cp    x-target/vbcc_target_m68k-amigaos/config/* "$DEV/vbcc/config/"
cp -r x-target/vbcc_target_m68k-amigaos/targets  "$DEV/vbcc/"
chmod +x "$DEV/vbcc/bin/"*

mkdir -p "$DEV/NDK3.2"
cp -r x-ndk/Include_H x-ndk/Include_I x-ndk/FD x-ndk/SFD x-ndk/lib \
      x-ndk/ReadMe-NDK.txt "$DEV/NDK3.2/"

rm -rf x-bin x-target x-ndk
echo "Development drawer ready at $DEV"
