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

Backends:
  - OPENROUTER_API_KEY set  -> routes via OpenRouter (OpenAI-compatible), so you
    can test with cheap models. Set OPENROUTER_MODEL (default anthropic/claude-haiku-4.5;
    e.g. anthropic/claude-sonnet-4.5). The proxy translates Anthropic Messages
    <-> OpenAI chat/completions both ways, including the tool-use round trip, so
    the Amiga client is unchanged.
  - else ANTHROPIC_API_KEY set -> calls api.anthropic.com directly.
  The key (either kind) stays on this host and is never sent to the Amiga.

Usage:
  OPENROUTER_API_KEY=sk-or-... python3 claude_amiga_proxy.py            # test via OpenRouter
  ANTHROPIC_API_KEY=sk-ant-... python3 claude_amiga_proxy.py           # direct Anthropic
  python3 claude_amiga_proxy.py --selftest      # one real round-trip if a key is set
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


OPENROUTER_URL = "https://openrouter.ai/api/v1/chat/completions"
OPENROUTER_MODEL = os.environ.get("OPENROUTER_MODEL", "anthropic/claude-haiku-4.5")


def _anthropic_to_openai(payload):
    """Translate an Anthropic Messages payload to OpenAI chat/completions.

    Handles plain text turns and the tool-use round trip: Anthropic tool_use
    blocks become OpenAI assistant tool_calls, and tool_result blocks become
    OpenAI role=tool messages.
    """
    out = []
    for m in payload["messages"]:
        role = m["role"]
        content = m["content"]
        if isinstance(content, str):
            out.append({"role": role, "content": content})
            continue
        # content is a list of blocks
        text_parts = []
        tool_calls = []
        tool_results = []
        for b in content:
            t = b.get("type")
            if t == "text":
                text_parts.append(b.get("text", ""))
            elif t == "tool_use":
                tool_calls.append({
                    "id": b.get("id", ""),
                    "type": "function",
                    "function": {"name": b.get("name", ""),
                                 "arguments": json.dumps(b.get("input", {}))},
                })
            elif t == "tool_result":
                c = b.get("content", "")
                if isinstance(c, list):
                    c = "".join(x.get("text", "") for x in c if isinstance(x, dict))
                tool_results.append({"role": "tool",
                                     "tool_call_id": b.get("tool_use_id", ""),
                                     "content": c})
        if tool_results:
            out.extend(tool_results)
        elif tool_calls:
            msg = {"role": "assistant", "content": " ".join(text_parts) or None}
            msg["tool_calls"] = tool_calls
            out.append(msg)
        else:
            out.append({"role": role, "content": " ".join(text_parts)})
    body = {"model": OPENROUTER_MODEL,
            "max_tokens": payload.get("max_tokens", DEFAULT_MAX_TOKENS),
            "messages": out}
    if "tools" in payload:
        body["tools"] = [{"type": "function",
                          "function": {"name": t["name"],
                                       "description": t.get("description", ""),
                                       "parameters": t.get("input_schema", {})}}
                         for t in payload["tools"]]
    return body


def _openai_to_anthropic(oai):
    """Translate an OpenAI chat/completions response into Anthropic shape so
    the Amiga client's parser (content[] text/tool_use, stop_reason) works."""
    choice = (oai.get("choices") or [{}])[0]
    msg = choice.get("message", {})
    blocks = []
    if msg.get("content"):
        blocks.append({"type": "text", "text": msg["content"]})
    for tc in msg.get("tool_calls") or []:
        fn = tc.get("function", {})
        try:
            args = json.loads(fn.get("arguments", "{}"))
        except ValueError:
            args = {}
        blocks.append({"type": "tool_use", "id": tc.get("id", ""),
                       "name": fn.get("name", ""), "input": args})
    finish = choice.get("finish_reason", "stop")
    stop = "tool_use" if (msg.get("tool_calls")) else \
           ("max_tokens" if finish == "length" else "end_turn")
    return {"id": oai.get("id", "msg_or"), "type": "message", "role": "assistant",
            "model": oai.get("model", OPENROUTER_MODEL),
            "content": blocks or [{"type": "text", "text": ""}],
            "stop_reason": stop,
            "usage": {"input_tokens": (oai.get("usage") or {}).get("prompt_tokens", 0),
                      "output_tokens": (oai.get("usage") or {}).get("completion_tokens", 0)}}


def call_openrouter(api_key, payload):
    """Forward via OpenRouter (OpenAI-compatible). Returns (status, anthropic_json_bytes)."""
    body = json.dumps(_anthropic_to_openai(payload)).encode("utf-8")
    req = urllib.request.Request(OPENROUTER_URL, data=body, method="POST")
    req.add_header("authorization", "Bearer " + api_key)
    req.add_header("content-type", "application/json")
    req.add_header("http-referer", "https://github.com/Scottcjn/rustchain-amiga")
    req.add_header("x-title", "Claude on the Amiga")
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            oai = json.loads(r.read().decode("utf-8"))
        return 200, json.dumps(_openai_to_anthropic(oai)).encode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except urllib.error.URLError as e:
        msg = json.dumps({"type": "error", "error":
                          {"type": "proxy_upstream_error", "message": str(e.reason)}})
        return 502, msg.encode("utf-8")


def call_backend(payload):
    """Pick the backend: OpenRouter if OPENROUTER_API_KEY is set, else Anthropic."""
    or_key = os.environ.get("OPENROUTER_API_KEY", "")
    if or_key:
        return call_openrouter(or_key, payload)
    ak = os.environ.get("ANTHROPIC_API_KEY", "")
    if not ak:
        return 500, json.dumps({"type": "error", "error":
                                {"type": "no_key",
                                 "message": "set OPENROUTER_API_KEY or ANTHROPIC_API_KEY"}}).encode()
    return call_anthropic(ak, payload)


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

        nmsg = len(payload["messages"])
        backend = "openrouter" if os.environ.get("OPENROUTER_API_KEY") else "anthropic"
        log("FWD %s backend=%s model=%s msgs=%d tools=%s"
            % (ip, backend, payload["model"], nmsg, "yes" if "tools" in payload else "no"))
        status, body = call_backend(payload)
        log("  <- %d (%d bytes)" % (status, len(body)))
        self._send_raw(status, body)


def selftest():
    if not (os.environ.get("OPENROUTER_API_KEY") or os.environ.get("ANTHROPIC_API_KEY")):
        print("set OPENROUTER_API_KEY or ANTHROPIC_API_KEY to self-test")
        return 0
    payload = {
        "model": DEFAULT_MODEL,
        "max_tokens": 64,
        "messages": [{"role": "user",
                      "content": "Say hello from the Amiga in one short line."}],
    }
    status, body = call_backend(payload)
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
