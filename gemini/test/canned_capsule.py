#!/usr/bin/env python3
"""
canned_capsule.py - a tiny local Gemini capsule (real TLS) that serves a
fixed gemtext document for ANY request, so the in-guest Amiga test is
deterministic (no dependency on flaky public internet reachability from the
FS-UAE guest -> host -> real capsule path).

This is the "target" the host-side gemini_amiga_proxy.py bridge does real
TLS to, on 127.0.0.1. The Amiga client never talks to this directly -- it
goes gemini(guest) -> bsdsocket -> gemini_amiga_proxy.py bridge (host,
plain TCP) -> TLS -> this capsule (host, loopback). Self-signed cert,
which is normal for Gemini and is why the client/bridge default to
SSL_VERIFY_NONE.

Usage: python3 canned_capsule.py --port 19811 \
           --cert capsule_cert.pem --key capsule_key.pem
"""

import argparse
import socket
import ssl
import sys
import threading

CAPSULE_BODY = (
    "# Gemini on the Amiga\n"
    "\n"
    "This is a canned capsule served for the FS-UAE in-guest test.\n"
    "\n"
    "## Section Two\n"
    "\n"
    "=> gemini://geminiprotocol.net/ Gemini Project homepage\n"
    "=> /about About this capsule\n"
    "=> https://example.com/ An external (non-gemini) link\n"
    "\n"
    "* first list item\n"
    "* second list item\n"
    "\n"
    "> a quoted line\n"
    "\n"
    "```\n"
    "=> this looks like a link but is preformatted text\n"
    "```\n"
    "\n"
    "End of capsule.\n"
)


def log(msg):
    sys.stderr.write("[capsule] %s\n" % msg)
    sys.stderr.flush()


def handle(conn):
    try:
        conn.settimeout(10.0)
        # Read the request line (Gemini: "<url>\r\n", up to 1024 bytes)
        buf = b""
        while not buf.endswith(b"\r\n") and len(buf) < 1024:
            b = conn.recv(1)
            if not b:
                break
            buf += b
        log("request: %r" % buf)
        body = CAPSULE_BODY.encode("utf-8")
        header = b"20 text/gemini\r\n"
        conn.sendall(header + body)
    except (socket.error, OSError, ssl.SSLError) as e:
        log("error: %s" % e)
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19811)
    ap.add_argument("--cert", default="capsule_cert.pem")
    ap.add_argument("--key", default="capsule_key.pem")
    args = ap.parse_args()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(8)
    log("canned Gemini capsule listening on %s:%d (TLS)" % (args.bind, args.port))

    try:
        while True:
            raw, addr = srv.accept()
            try:
                tls = ctx.wrap_socket(raw, server_side=True)
            except ssl.SSLError as e:
                log("TLS handshake failed from %s: %s" % (addr[0], e))
                raw.close()
                continue
            t = threading.Thread(target=handle, args=(tls,), daemon=True)
            t.start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    main()
