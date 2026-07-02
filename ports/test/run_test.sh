#!/bin/bash
# amiport in-guest integration test.
#
# Serves ports/repo/ over HTTP on 127.0.0.1:8873, boots FS-UAE headless
# with ports/test/shared/ as the guest's SYS: volume, and verifies from
# the log the GUEST writes that list + install + run-installed-binary
# all happened inside the emulated Amiga.
#
# Prereqs: repo built (harness/amiport-build.py build-all), client built
# (make -C client amiport), fs-uae + xvfb-run installed, AROS ROMs in
# emu/roms/ (see emu/README.md).

set -u
PORTS="$(cd "$(dirname "$0")/.." && pwd)"
SHARED="$PORTS/test/shared"
LOG="$SHARED/amiport-test.log"
HTTP_PORT=8873
BOOT_TIMEOUT=180

fail() { echo "FAIL: $1"; exit 1; }

[ -f "$PORTS/repo/index.txt" ] || fail "repo not built (run harness/amiport-build.py build-all)"
[ -f "$PORTS/client/amiport" ] || fail "client not built (make -C client amiport)"

mkdir -p "$PORTS/test/screenshots"

# fresh guest volume state
rm -f "$LOG" "$SHARED"/*.uaem "$SHARED"/S/*.uaem 2>/dev/null
rm -rf "$SHARED/amiports"
cp "$PORTS/client/amiport" "$SHARED/amiport"

# repo server (bsdsocket guest shares the host stack, loopback works)
python3 -m http.server $HTTP_PORT --bind 0.0.0.0 --directory "$PORTS/repo" \
    >"$PORTS/test/httpd.log" 2>&1 &
HTTPD=$!
sleep 1
kill -0 $HTTPD 2>/dev/null || fail "http server did not start (port $HTTP_PORT busy?)"

# emulator, headless
timeout $((BOOT_TIMEOUT + 60)) xvfb-run --auto-servernum \
    -s "-screen 0 1280x1024x24" \
    fs-uae "$PORTS/test/amiport-test.fs-uae" \
    >"$PORTS/test/fsuae_run.log" 2>&1 &
FSUAE=$!

cleanup() {
    kill $FSUAE 2>/dev/null
    kill $HTTPD 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT

# poll for the guest's completion marker
echo "waiting for the guest (up to ${BOOT_TIMEOUT}s)..."
for i in $(seq $BOOT_TIMEOUT); do
    if grep -q "amiport test done" "$LOG" 2>/dev/null; then
        echo "guest finished after ${i}s"
        break
    fi
    kill -0 $FSUAE 2>/dev/null || break
    sleep 1
done

echo
echo "===== guest log ($LOG) ====="
cat "$LOG" 2>/dev/null || fail "guest never wrote $LOG"
echo "============================"
echo

rc=0
check() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        echo "PASS: $2"
    else
        echo "FAIL: $2"
        rc=1
    fi
}

check "amiport test boot OK"        "guest booted and ran startup-sequence"
check "packages in the repo"        "amiport list fetched the index over HTTP"
check "sha1 verified:"              "amiport install verified the archive SHA-1"
check "installed hello-amiports"    "amiport install extracted the package"
check "HELLO_AMIPORTS_PROOF_V1"     "the INSTALLED binary ran inside the guest"
check "installed rustchain-tools"   "multi-file prebuilt package installed"
check "hello-amiports|1.0|"         "package registered in installed.db"
check "rustchain-tools|1.0|"        "rustchain-tools registered in installed.db"
check "amiport test done"           "startup-sequence completed"

[ -f "$SHARED/amiports/installed.db" ] \
    && echo "PASS: installed.db exists on the guest volume" \
    || { echo "FAIL: installed.db missing"; rc=1; }
[ -f "$SHARED/amiports/hello-amiports/hello" ] \
    && echo "PASS: installed binary exists on the guest volume" \
    || { echo "FAIL: installed hello binary missing"; rc=1; }

echo
[ $rc -eq 0 ] && echo "IN-GUEST TEST PASSED" || echo "IN-GUEST TEST FAILED"
exit $rc
