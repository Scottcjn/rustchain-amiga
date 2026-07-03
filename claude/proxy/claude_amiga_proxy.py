#!/usr/bin/env python3
"""
claude_amiga_proxy.py - host-side TLS/key bridge for Claude on the Amiga.

This is the OPTIONAL FALLBACK transport. The Amiga client (claude.c) talks
HTTPS to api.anthropic.com directly when AmiSSL is installed. On machines
with no TLS library (a bare-ROM AROS boot, an old AmiTCP-only setup) the
client instead POSTs plain HTTP to this proxy with `claude --proxy host:port`,
and the proxy does the TLS and holds the API key.

Design (mirrors miner_proxy_secure.py):
  - Plain HTTP on 0.0.0.0:8790 (reachable on the LAN and from FS-UAE's
    bsdsocket, which shares the host's network identity).
  - POST /  body {"messages":[...], "model"?:..., "max_tokens"?:..., "tools"?:...}
  - Reads the real key from env ANTHROPIC_API_KEY. Never sends it to the Amiga.
  - Calls https://api.anthropic.com/v1/messages (urllib does the TLS), adding
    x-api-key + anthropic-version, and returns the Anthropic JSON verbatim.
    The Amiga runs the tool-use loop itself.
  - IP allowlist, simple per-IP rate limit, audit log to stderr.

Usage:
  ANTHROPIC_API_KEY=sk-ant-... python3 claude_amiga_proxy.py
  python3 claude_amiga_proxy.py --selftest      # one real round-trip if key set
  python3 claude_amiga_proxy.py --port 8790 --bind 0.0.0.0

Standard library only. No pip.
"""

import sys
import os
import json
import time
import argparse
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ANTHROPIC_URL = "https://api.anthropic.com/v1/messages"
ANTHROPIC_VERSION = "2023-06-01"
DEFAULT_MODEL = "claude-opus-4-8"
DEFAULT_MAX_TOKENS = 4096
MAX_BODY = 2 * 1024 * 1024   # 2 MB request cap

# IP allowlist prefixes: local LAN + loopback + Tailscale CGNAT ranges.
ALLOWED_PREFIXES = (
    "192.168.",
    "127.0.0.1",
    "::1",
    "10.",
    "100.64.", "100.75.", "100.88.", "100.94.", "100.95.",
)

# crude per-IP rate limit
RATE_WINDOW = 60.0
RATE_MAX = 30
_rate = {}


def log(msg):
    sys.stderr.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stderr.flush()


def ip_allowed(ip):
    return any(ip.startswith(p) for p in ALLOWED_PREFIXES)


def rate_ok(ip):
    now = time.time()
    hits = [t for t in _rate.get(ip, []) if now - t < RATE_WINDOW]
    if len(hits) >= RATE_MAX:
        _rate[ip] = hits
        return False
    hits.append(now)
    _rate[ip] = hits
    return True


def call_anthropic(api_key, payload):
    """Forward payload to Anthropic over TLS. Returns (status, body_bytes)."""
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(ANTHROPIC_URL, data=body, method="POST")
    req.add_header("x-api-key", api_key)
    req.add_header("anthropic-version", ANTHROPIC_VERSION)
    req.add_header("content-type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        # forward the API's own error body verbatim (still useful to the Amiga)
        return e.code, e.read()
    except urllib.error.URLError as e:
        msg = json.dumps({"type": "error",
                          "error": {"type": "proxy_upstream_error",
                                    "message": str(e.reason)}})
        return 502, msg.encode("utf-8")


def build_payload(req_json):
    """Assemble the Anthropic request from the Amiga's request."""
    if "messages" not in req_json or not isinstance(req_json["messages"], list):
        return None, "request needs a 'messages' array"
    payload = {
        "model": req_json.get("model", DEFAULT_MODEL),
        "max_tokens": int(req_json.get("max_tokens", DEFAULT_MAX_TOKENS)),
        "messages": req_json["messages"],
    }
    if "tools" in req_json and req_json["tools"]:
        payload["tools"] = req_json["tools"]
    if "system" in req_json and req_json["system"]:
        payload["system"] = req_json["system"]
    return payload, None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # force per-request close so blocking clients see EOF

    def log_message(self, fmt, *args):
        pass  # we do our own audit logging

    def _send_json(self, status, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(data)

    def _send_raw(self, status, data):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        # a tiny health endpoint so `curl host:8790/health` works
        if self.path == "/health":
            self._send_json(200, {"ok": True, "service": "claude-amiga-proxy"})
        else:
            self._send_json(404, {"error": "POST / with a messages array"})

    def do_POST(self):
        ip = self.client_address[0]
        if not ip_allowed(ip):
            log("DENY %s (not in allowlist)" % ip)
            self._send_json(403, {"error": "forbidden"})
            return
        if not rate_ok(ip):
            log("RATE-LIMIT %s" % ip)
            self._send_json(429, {"error": "rate limited"})
            return

        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self._send_json(400, {"error": "missing or oversized body"})
            return

        raw = self.rfile.read(length)
        try:
            req_json = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self._send_json(400, {"error": "invalid JSON"})
            return

        payload, err = build_payload(req_json)
        if err:
            self._send_json(400, {"error": err})
            return

        api_key = os.environ.get("ANTHROPIC_API_KEY", "")
        if not api_key:
            log("NO KEY set; refusing to call upstream")
            self._send_json(500, {"error": "proxy has no ANTHROPIC_API_KEY set",
                                  "hint": "start the proxy with the key in env"})
            return

        nmsg = len(payload["messages"])
        log("FWD %s model=%s msgs=%d tools=%s"
            % (ip, payload["model"], nmsg, "yes" if "tools" in payload else "no"))
        status, body = call_anthropic(api_key, payload)
        log("  <- %d (%d bytes)" % (status, len(body)))
        self._send_raw(status, body)


def selftest():
    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        print("set ANTHROPIC_API_KEY to self-test")
        return 0
    payload = {
        "model": DEFAULT_MODEL,
        "max_tokens": 64,
        "messages": [{"role": "user",
                      "content": "Say hello from the Amiga in one short line."}],
    }
    print("calling %s ..." % ANTHROPIC_URL)
    status, body = call_anthropic(api_key, payload)
    print("HTTP %d" % status)
    try:
        obj = json.loads(body.decode("utf-8"))
        for block in obj.get("content", []):
            if block.get("type") == "text":
                print("Claude: %s" % block["text"])
        print("stop_reason: %s" % obj.get("stop_reason"))
    except (ValueError, UnicodeDecodeError):
        print(body[:500])
    return 0


def main():
    ap = argparse.ArgumentParser(description="Claude on the Amiga host proxy")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not os.environ.get("ANTHROPIC_API_KEY"):
        log("WARNING: ANTHROPIC_API_KEY is not set; requests will get a 500 "
            "until you set it. The key stays on this host and is never sent "
            "to the Amiga.")

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    log("Claude Amiga proxy listening on %s:%d (POST /). Ctrl-C to stop."
        % (args.bind, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
