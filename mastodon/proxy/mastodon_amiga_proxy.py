#!/usr/bin/env python3
"""
mastodon_amiga_proxy.py - host-side plain-HTTP bridge for the Mastodon
client on the Amiga (mastodon.c), used with `mastodon --proxy host:port`.

This is the OPTIONAL FALLBACK transport. The Amiga client talks HTTPS
straight to the instance when AmiSSL is installed (the primary path). On a
machine with no TLS library, or just for testing, the client instead POSTs
plain HTTP to this bridge, which does the real TLS call (or, in --mock mode,
just hands back a canned response so the whole client can be proven working
with no real instance and no real token at all).

Protocol (mirrors claude_amiga_proxy.py's shape, adapted since this client
has two different operations instead of one):
  POST / with a JSON envelope built by the Amiga:
    {"method":"GET"|"POST", "host":"...", "path":"...", "token":"...",
     "body":"..."}          (body only present for POST)
  The Amiga builds this itself; it decides method/path/token/body. This
  bridge is a relay, not a Mastodon client of its own: in relay mode it does
  not hold or need its own token, it just forwards whatever Authorization
  the Amiga asked for to the real instance over TLS, and returns that
  instance's real status and body verbatim as its own HTTP response (which
  is exactly what the Amiga's http_status()/http_body() parser expects,
  same as if it had done the TLS itself).

Modes:
  relay (default) - actually calls https://{host}{path} via urllib (real
    TLS, real instance, real token if one was sent). Needs a real Mastodon
    account and token to be useful past the public timeline.
  --mock - ignores host/token entirely and returns a canned timeline / a
    canned "created" response for /api/v1/statuses, echoing back whatever
    status text was posted. This is how the client gets proven end to end
    without a real instance, real account, or real token: see ../README.md.

Standard library only. No pip.
"""

import sys
import os
import re
import json
import time
import html
import argparse
import urllib.request
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY = 1 * 1024 * 1024   # 1 MB envelope cap

# IP allowlist prefixes: local LAN + loopback + Tailscale CGNAT ranges,
# same list as claude_amiga_proxy.py.
ALLOWED_PREFIXES = (
    "192.168.",
    "127.0.0.1",
    "::1",
    "10.",
    "100.64.", "100.75.", "100.88.", "100.94.", "100.95.",
)

RATE_WINDOW = 60.0
RATE_MAX = 30
_rate = {}

MOCK_TIMELINE = [
    {
        "id": "1001",
        "created_at": "2026-01-01T00:00:00.000Z",
        "content": "<p>Hello from the mock Fediverse! Testing "
                   "<a href=\"#\">@mastodon</a> on an Amiga.</p>",
        "account": {"id": "1", "username": "alice", "acct": "alice",
                    "display_name": "Alice A."},
        "reblog": None,
    },
    {
        "id": "1002",
        "created_at": "2026-01-01T00:05:00.000Z",
        "content": "",
        "account": {"id": "2", "username": "bob", "acct": "bob@other.example",
                    "display_name": "Bob"},
        "reblog": {
            "id": "999",
            "content": "<p>Boosted toot: retro computing rules.<br>"
                       "Second line after a break.</p>",
            "account": {"id": "3", "username": "carol",
                        "acct": "carol@retro.example",
                        "display_name": "Carol C"},
        },
    },
    {
        "id": "1003",
        "created_at": "2026-01-01T00:10:00.000Z",
        "content": "<p>No AmiSSL needed for this one, it came through "
                   "the mock bridge.</p>",
        "account": {"id": "4", "username": "dave", "acct": "dave",
                    "display_name": "Dave on a 68030"},
        "reblog": None,
    },
]


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


def strip_html_for_echo(text):
    """Same-shape stripping as the Amiga client's html_strip, good enough
    for echoing a posted status back in a mock response."""
    text = re.sub(r"(?i)<br\s*/?>", "\n", text)
    text = re.sub(r"(?i)</p>", "\n", text)
    text = re.sub(r"<[^>]*>", "", text)
    return html.unescape(text).strip()


def mock_response(method, path, body_json):
    """Return (status, body_bytes) for --mock mode. body_json is the parsed
    JSON that the Amiga wanted sent upstream (None for GET)."""
    if method == "GET" and path in ("/api/v1/timelines/home",
                                    "/api/v1/timelines/public"):
        return 200, json.dumps(MOCK_TIMELINE).encode("utf-8")

    if method == "POST" and path == "/api/v1/statuses":
        status_text = ""
        if isinstance(body_json, dict):
            status_text = body_json.get("status", "")
        reply = {
            "id": "2001",
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime()),
            "content": "<p>%s</p>" % html.escape(status_text),
            "url": "https://mock.example/@amiga/2001",
            "account": {"id": "42", "acct": "amiga",
                        "display_name": "An Actual Amiga"},
        }
        log("MOCK created status: %r" % strip_html_for_echo(reply["content"]))
        return 200, json.dumps(reply).encode("utf-8")

    return 404, json.dumps({"error": "mock: no route for %s %s" % (method, path)}).encode("utf-8")


def relay_request(method, host, path, token, body_str):
    """Real relay mode: perform the actual TLS call to the instance."""
    url = "https://%s%s" % (host, path)
    data = body_str.encode("utf-8") if (method == "POST" and body_str) else None
    req = urllib.request.Request(url, data=data, method=method)
    if token:
        req.add_header("Authorization", "Bearer %s" % token)
    if method == "POST":
        req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", "mastodon-amiga-proxy/1.0")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except urllib.error.URLError as e:
        msg = json.dumps({"error": "proxy_upstream_error: %s" % e.reason})
        return 502, msg.encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"   # force per-request close, matches claude's proxy
    mock_mode = False

    def log_message(self, fmt, *args):
        pass

    def _send_raw(self, status, data):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/health":
            self._send_raw(200, json.dumps({"ok": True, "mock": self.mock_mode}).encode())
        else:
            self._send_raw(404, json.dumps({"error": "POST / with an envelope"}).encode())

    def do_POST(self):
        ip = self.client_address[0]
        if not ip_allowed(ip):
            log("DENY %s (not in allowlist)" % ip)
            self._send_raw(403, json.dumps({"error": "forbidden"}).encode())
            return
        if not rate_ok(ip):
            log("RATE-LIMIT %s" % ip)
            self._send_raw(429, json.dumps({"error": "rate limited"}).encode())
            return

        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_BODY:
            self._send_raw(400, json.dumps({"error": "missing or oversized body"}).encode())
            return

        raw = self.rfile.read(length)
        try:
            envelope = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self._send_raw(400, json.dumps({"error": "invalid JSON envelope"}).encode())
            return

        method = envelope.get("method", "GET")
        host = envelope.get("host", "")
        path = envelope.get("path", "/")
        token = envelope.get("token", "")
        body_str = envelope.get("body", "")

        log("%s %s host=%s path=%s token=%s body_len=%d"
            % ("MOCK" if self.mock_mode else "RELAY", method, host, path,
               "yes" if token else "no", len(body_str)))

        if self.mock_mode:
            body_json = None
            if body_str:
                try:
                    body_json = json.loads(body_str)
                except ValueError:
                    body_json = None
            status, out = mock_response(method, path, body_json)
        else:
            if not host:
                self._send_raw(400, json.dumps({"error": "envelope missing host"}).encode())
                return
            status, out = relay_request(method, host, path, token, body_str)

        log("  <- %d (%d bytes)" % (status, len(out)))
        self._send_raw(status, out)


def main():
    ap = argparse.ArgumentParser(description="Mastodon on the Amiga host bridge")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8791)
    ap.add_argument("--mock", action="store_true",
                    help="serve canned timeline/post responses, no real instance needed")
    args = ap.parse_args()

    Handler.mock_mode = args.mock

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    log("Mastodon Amiga bridge listening on %s:%d (POST /), mock=%s. Ctrl-C to stop."
        % (args.bind, args.port, args.mock))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
