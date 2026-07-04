#!/bin/bash
# run_test.sh - Gemini-on-the-Amiga in-guest integration test.
#
# Starts a canned local Gemini capsule (real TLS, fixed gemtext body) and the
# host proxy bridge (gemini/proxy/gemini_amiga_proxy.py, no API key / no
# public internet needed), boots AROS m68k headless in FS-UAE, lets
# S/startup-sequence run the gemini client (proxy-only build, no AmiSSL)
# through the bridge, and verifies from the host that the rendered gemtext
# (headings + numbered link index) reached SYS:gemini.log.
#
# PARALLELISM: other agents run FS-UAE concurrently. This script uses ONLY
# Xvfb display :81 and host port 8811 (mandated), plus a private capsule
# port 19811 that is never exposed past 127.0.0.1. It never broadly pkills
# fs-uae/Xvfb -- only the specific PIDs it started, and a stale :81 Xvfb if
# present before starting.
#
# Usage: ./run_test.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SHARED="$HERE/shared"
CFG="$HERE/gemini.fs-uae"
BRIDGE_PORT=8811
CAPSULE_PORT=19811
DISPLAY_NUM=":81"
LOG="$SHARED/gemini.log"
DONE="$SHARED/gemini_done.txt"
SCREENSHOT="$HERE/screenshots/gemini.png"

echo "== Gemini on the Amiga: in-guest test =="

# 0. Kill only a STALE Xvfb on our private display :81, if one is lingering
#    from a previous run of this same test. Do NOT touch fs-uae or other
#    Xvfb displays -- sibling agents are running concurrently.
STALE_XVFB="$(pgrep -f "Xvfb $DISPLAY_NUM " 2>/dev/null || true)"
if [ -n "$STALE_XVFB" ]; then
    echo "killing stale Xvfb $DISPLAY_NUM (pid $STALE_XVFB)"
    kill -9 $STALE_XVFB 2>/dev/null
    sleep 1
fi

# 1. clean prior output (and FS-UAE .uaem sidecars) so results are fresh
rm -f "$LOG" "$LOG.uaem" "$DONE" "$DONE.uaem"
mkdir -p "$HERE/screenshots"

# 2. start the canned local Gemini capsule (real TLS, fixed body, loopback
#    only -- the bridge is the only thing that ever talks to it)
python3 "$HERE/canned_capsule.py" --bind 127.0.0.1 --port "$CAPSULE_PORT" \
    --cert "$HERE/capsule_cert.pem" --key "$HERE/capsule_key.pem" \
    >/tmp/gemini_capsule.log 2>&1 &
CAPSULE=$!
sleep 1
if ! kill -0 $CAPSULE 2>/dev/null; then
    echo "FAIL: canned capsule did not start"; cat /tmp/gemini_capsule.log; exit 1
fi
echo "canned capsule up on 127.0.0.1:$CAPSULE_PORT (pid $CAPSULE)"

# 3. start the proxy bridge (bound 0.0.0.0 so guest bsdsocket loopback
#    reaches it; it does real TLS out to the capsule above)
python3 "$HERE/../proxy/gemini_amiga_proxy.py" --bind 0.0.0.0 --port "$BRIDGE_PORT" \
    >/tmp/gemini_bridge.log 2>&1 &
BRIDGE=$!
sleep 1
if ! kill -0 $BRIDGE 2>/dev/null; then
    echo "FAIL: proxy bridge did not start"; cat /tmp/gemini_bridge.log
    kill -9 $CAPSULE 2>/dev/null; exit 1
fi
echo "proxy bridge up on 0.0.0.0:$BRIDGE_PORT (pid $BRIDGE)"

# 4. start our OWN Xvfb on display :81 (private to this test)
Xvfb $DISPLAY_NUM -screen 0 1024x768x24 &
XVFB=$!
sleep 1
export DISPLAY=$DISPLAY_NUM
echo "Xvfb $DISPLAY_NUM up (pid $XVFB)"

# 5. boot headless in the background, capture OUR fs-uae pid only
fs-uae "$CFG" >/tmp/gemini_fsuae.log 2>&1 &
FPID=$!
echo "fs-uae booting (pid $FPID) ..."

# 6. poll for the guest to finish (up to ~180s), grabbing a screenshot once
#    the log has content
WAITED=0
SHOT_TAKEN=0
while [ $WAITED -lt 180 ]; do
    if [ -f "$DONE" ]; then
        echo "guest signaled done at t=${WAITED}s"
        break
    fi
    if [ "$SHOT_TAKEN" = "0" ] && [ -f "$LOG" ] && [ -s "$LOG" ] && [ $WAITED -ge 20 ]; then
        import -window root -display "$DISPLAY_NUM" "$SCREENSHOT" 2>/tmp/gemini_import.log
        SHOT_TAKEN=1
        echo "mid-run screenshot captured at t=${WAITED}s"
    fi
    if ! kill -0 $FPID 2>/dev/null; then
        echo "fs-uae process exited early at t=${WAITED}s"
        break
    fi
    sleep 3
    WAITED=$((WAITED + 3))
done

# 7. final screenshot regardless (boot console / whatever is on screen now)
import -window root -display "$DISPLAY_NUM" "$SCREENSHOT" 2>/tmp/gemini_import.log
echo "final screenshot: $SCREENSHOT"

# 8. stop only what we started
kill -9 $FPID 2>/dev/null
sleep 1
kill -9 $XVFB 2>/dev/null
pkill -9 -f "Xvfb $DISPLAY_NUM " 2>/dev/null
kill -9 $BRIDGE 2>/dev/null
kill -9 $CAPSULE 2>/dev/null

# 9. verdict
echo
echo "== evidence =="
PASS=1

if [ -f "$LOG" ]; then
    echo "--- SYS:gemini.log ---"
    cat "$LOG"
    echo "----------------------"
    if grep -q "Gemini on the Amiga" "$LOG"; then
        echo "PASS: rendered gemtext heading reached the guest log"
    else
        echo "FAIL: rendered gemtext heading not found in gemini.log"; PASS=0
    fi
    if grep -q "Gemini Project homepage" "$LOG"; then
        echo "PASS: rendered link label reached the guest log"
    else
        echo "FAIL: rendered link label not found in gemini.log"; PASS=0
    fi
else
    echo "FAIL: gemini.log was never created (guest did not boot/run)"; PASS=0
fi

if [ -f "$DONE" ]; then
    echo "PASS: guest wrote $DONE"
else
    echo "FAIL: guest did not signal done"; PASS=0
fi

echo
echo "--- capsule log ---"; cat /tmp/gemini_capsule.log
echo
echo "--- bridge log ---"; cat /tmp/gemini_bridge.log
echo

if [ "$PASS" = "1" ]; then
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
