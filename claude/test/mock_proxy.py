#!/usr/bin/env python3
"""
mock_proxy.py - a canned Anthropic-shaped endpoint for the in-guest FS-UAE
test. NO API key, NO network to Anthropic. It speaks the same plain-HTTP
contract as claude_amiga_proxy.py so the Amiga `claude --proxy host:port`
client can be exercised end to end offline.

Behaviour (stateful by inspecting the request):
  1. If any message contains a tool_result block -> reply stop_reason end_turn
     with a short confirmation. (This closes the tool-use loop.)
  2. Else if the conversation mentions TOOLTEST -> reply stop_reason tool_use
     asking the client to write_file SYS:claude_tool_out.txt. (This drives the
     tool-use loop; the Amiga executes write_file locally.)
  3. Else -> reply stop_reason end_turn with a plain hello.

Usage: python3 mock_proxy.py --port 8791
"""

import sys
import json
import argparse
from http.server import BaseHTTPRequestHandler, HTTPServer

TOOL_FILE = "SYS:claude_tool_out.txt"
TOOL_TEXT = "Hello, written by the Claude tool-use loop on the Amiga.\n"


def log(msg):
    sys.stderr.write("[mock] %s\n" % msg)
    sys.stderr.flush()


def contains_tool_result(messages):
    for m in messages:
        c = m.get("content")
        if isinstance(c, list):
            for b in c:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    return True
        elif isinstance(c, str) and "tool_result" in c:
            return True
    return False


def mentions_tooltest(messages):
    for m in messages:
        c = m.get("content")
        if isinstance(c, str) and "TOOLTEST" in c:
            return True
        if isinstance(c, list):
            for b in c:
                if isinstance(b, dict) and "TOOLTEST" in json.dumps(b):
                    return True
    return False


def reply_text(text):
    return {
        "id": "msg_mock_text",
        "type": "message",
        "role": "assistant",
        "model": "claude-opus-4-8-mock",
        "content": [{"type": "text", "text": text}],
        "stop_reason": "end_turn",
        "usage": {"input_tokens": 1, "output_tokens": 1},
    }


def reply_tool_use():
    return {
        "id": "msg_mock_tool",
        "type": "message",
        "role": "assistant",
        "model": "claude-opus-4-8-mock",
        "content": [
            {"type": "text", "text": "Sure, I will write that file for you."},
            {"type": "tool_use",
             "id": "toolu_mock_0001",
             "name": "write_file",
             "input": {"path": TOOL_FILE, "text": TOOL_TEXT}},
        ],
        "stop_reason": "tool_use",
        "usage": {"input_tokens": 1, "output_tokens": 1},
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"  # force per-request close so blocking clients see EOF

    def log_message(self, fmt, *args):
        pass

    def _send(self, obj):
        data = json.dumps(obj).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(data)

    def do_GET(self):
        self._send({"ok": True, "service": "claude-mock-proxy"})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length > 0 else b"{}"
        try:
            req = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            req = {}
        messages = req.get("messages", [])

        if contains_tool_result(messages):
            log("saw tool_result -> end_turn confirmation")
            self._send(reply_text(
                "Done. The file was written on the Amiga by the tool-use loop."))
        elif mentions_tooltest(messages):
            log("TOOLTEST -> tool_use write_file")
            self._send(reply_tool_use())
        else:
            log("plain prompt -> hello")
            self._send(reply_text("Hello from the Amiga! (mock reply)"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8791)
    args = ap.parse_args()
    srv = HTTPServer((args.bind, args.port), Handler)
    log("mock proxy on %s:%d" % (args.bind, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
