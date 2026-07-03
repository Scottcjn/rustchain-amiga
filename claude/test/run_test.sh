#!/bin/bash
# run_test.sh - Claude on the Amiga in-guest integration test.
#
# Starts a host mock proxy (no API key needed), boots AROS m68k headless in
# FS-UAE, lets S/startup-sequence run the claude client through the proxy, and
# verifies from the host that:
#   1. the client got the mock chat reply into SYS:claude.log, and
#   2. the tool-use loop wrote SYS:claude_tool_out.txt in shared/.
#
# Usage: ./run_test.sh
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SHARED="$HERE/shared"
CFG="$HERE/claude-test.fs-uae"
PORT=8791
LOG="$SHARED/claude.log"
TOOLOUT="$SHARED/claude_tool_out.txt"
DONE="$SHARED/claude_done.txt"

echo "== Claude Amiga in-guest test =="

# 0. kill any stale emulator / X server / mock (these have caused hangs and
#    port collisions before)
pkill -9 -f fs-uae 2>/dev/null
pkill -9 Xvfb 2>/dev/null
pkill -9 -f mock_proxy.py 2>/dev/null
sleep 1

# 1. clean prior output (and FS-UAE .uaem sidecars) so results are fresh
rm -f "$LOG" "$LOG.uaem" "$TOOLOUT" "$TOOLOUT.uaem" "$DONE" "$DONE.uaem"

# 1b. point the guest at 127.0.0.1. FS-UAE's bsdsocket runs the guest's socket
#     calls as HOST socket calls, so the guest connecting to 127.0.0.1 reaches
#     the HOST loopback where the mock listens. This is the proven pattern from
#     the amiports test (ports/test/run_test.sh serves on 0.0.0.0, guest hits
#     127.0.0.1). An earlier LAN-IP rewrite here hung tcp_connect -- reverted.
echo "guest proxy target: 127.0.0.1:$PORT (bsdsocket -> host loopback)"
sed -i -E "s/--proxy [0-9.]+:$PORT/--proxy 127.0.0.1:$PORT/g" \
    "$SHARED/S/startup-sequence"

# 2. start the mock proxy (bound to 0.0.0.0 so both loopback and LAN reach it)
python3 "$HERE/mock_proxy.py" --bind 0.0.0.0 --port "$PORT" >/tmp/claude_mock.log 2>&1 &
MOCK=$!
sleep 1
if ! curl -s "http://127.0.0.1:$PORT/" >/dev/null; then
    echo "FAIL: mock proxy did not start"; cat /tmp/claude_mock.log; kill $MOCK 2>/dev/null; exit 1
fi
echo "mock proxy up on 127.0.0.1:$PORT (pid $MOCK)"

# 3. boot headless (timeout guards against a hung emulator)
echo "booting AROS headless (up to 200s) ..."
timeout 200 xvfb-run -a -s "-screen 0 1024x768x24" \
    fs-uae "$CFG" >/tmp/claude_fsuae.log 2>&1

# 4. give the guest a moment to flush files, then stop everything
sleep 2
pkill -9 -f fs-uae 2>/dev/null
kill $MOCK 2>/dev/null
sleep 1

# 5. verdict
echo
echo "== evidence =="
PASS=1

if [ -f "$LOG" ]; then
    echo "--- SYS:claude.log ---"
    cat "$LOG"
    echo "----------------------"
    if grep -q "Hello from the Amiga" "$LOG"; then
        echo "PASS: chat reply reached the guest"
    else
        echo "FAIL: chat reply not found in claude.log"; PASS=0
    fi
else
    echo "FAIL: claude.log was never created (guest did not boot/run)"; PASS=0
fi

if [ -f "$TOOLOUT" ]; then
    echo "PASS: tool-use loop wrote $TOOLOUT :"
    echo "  \"$(cat "$TOOLOUT")\""
else
    echo "FAIL: tool-use output file was not written"; PASS=0
fi

echo
echo "--- mock proxy log ---"; cat /tmp/claude_mock.log
echo

if [ "$PASS" = "1" ]; then
    echo "RESULT: PASS"
    exit 0
else
    echo "RESULT: FAIL"
    exit 1
fi
