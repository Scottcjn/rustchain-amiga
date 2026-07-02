#!/usr/bin/env bash
# Headless boot test of the public RustChainAmiga.hdf.
# Boots a throwaway copy (with S/Test-Hooks injected) under the AROS ROM in
# FS-UAE + Xvfb, then verifies the guest wrote proof to test/shared/ and that
# the miner dry-run log inside the image is sane.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
D="$(dirname "$HERE")"
XDF="$D/venv/bin/xdftool"
HDF="$D/images/RustChainAmiga.hdf"
TESTHDF="$HERE/RustChainAmiga-test.hdf"

[ -f "$HDF" ] || { echo "run ../assemble.sh first"; exit 1; }

echo "[boot-test] preparing throwaway test image with S/Test-Hooks"
cp "$HDF" "$TESTHDF"
"$XDF" "$TESTHDF" write "$D/scripts/test-hooks" S/Test-Hooks

mkdir -p "$HERE/shared" "$HERE/screenshots"
rm -f "$HERE/shared/distro_boot.log" "$HERE/shared/miner-dryrun.log" "$HERE/shared/"*.uaem

echo "[boot-test] booting headless (timeout 180s; timeout kill is expected)"
timeout 180 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
    fs-uae "$HERE/rustchain-distro.fs-uae" > "$HERE/boot_test.log" 2>&1 || true

echo "[boot-test] results:"
FAIL=0
if [ -f "$HERE/shared/distro_boot.log" ]; then
    echo "--- shared/distro_boot.log (written by the guest) ---"
    cat "$HERE/shared/distro_boot.log"
else
    echo "FAIL: guest did not write shared/distro_boot.log"; FAIL=1
fi
if [ -f "$HERE/shared/miner-dryrun.log" ]; then
    echo "--- shared/miner-dryrun.log (first 20 lines) ---"
    head -20 "$HERE/shared/miner-dryrun.log"
else
    echo "WARN: no miner-dryrun.log (miner missing from image?)"
fi
echo "--- extracting SYS:T/rustchain-lastrun.log from test image ---"
"$XDF" "$TESTHDF" read T/rustchain-lastrun.log "$HERE/rustchain-lastrun.log" 2>/dev/null \
    && head -10 "$HERE/rustchain-lastrun.log" \
    || echo "WARN: no T/rustchain-lastrun.log inside image"

[ "$FAIL" -eq 0 ] && echo "[boot-test] PASS: public HDF boots under AROS ROM and ran its startup-sequence" \
                  || { echo "[boot-test] FAIL"; exit 1; }
