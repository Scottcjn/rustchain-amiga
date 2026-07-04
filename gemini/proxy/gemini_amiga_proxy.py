#!/usr/bin/env python3
"""
gemini_amiga_proxy.py - host-side TLS bridge for the Gemini client on the
Amiga (gemini.c).

This is the OPTIONAL FALLBACK transport. The Amiga client talks TLS to the
target Gemini capsule directly when AmiSSL is installed. On a machine with
no TLS library (a bare-ROM AROS boot, an AmiTCP-only setup, or just for
testing without setting up AmiSSL in an emulator) the client instead opens a
plain TCP connection to this bridge with `gemini --proxy host:port
gemini://...`, and the bridge does the TLS.

Wire protocol (deliberately dumb, no framing beyond two lines):
  1. The Amiga connects to this bridge over plain TCP and sends one line,
     LF-terminated: "<target-host> <target-port>\n"
  2. It then sends the exact Gemini request line it would have sent over
     TLS directly: "<url>\r\n" (per the Gemini spec, max 1024 bytes).
  3. This bridge opens a TLS connection to <target-host>:<target-port>,
     forwards that request line verbatim, and relays every byte the target
     sends back to the Amiga, raw, until the target closes the connection
     (Gemini has no length framing -- close-to-end is how the protocol
     itself signals "done"). Then it closes both sockets.

Because the bridge relays raw bytes both ways with no reinterpretation, the
Amiga's response-parsing code (gem_status_code/gem_status_meta/gem_body/
gem_render_gemtext) is identical whether it came from AmiSSL directly or
through this bridge. TOFU / certificate trust is NOT implemented here either
(see gemini.c's file header): this bridge does not verify the target's
certificate, matching the Amiga client's own default of trusting whatever
cert the capsule presents.

Usage:
  python3 gemini_amiga_proxy.py                     # listens on 0.0.0.0:8791
  python3 gemini_amiga_proxy.py --port 8791 --bind 0.0.0.0
  python3 gemini_amiga_proxy.py --selftest gemini://geminiprotocol.net/

Standard library only. No pip.
"""

import sys
import socket
import ssl
import argparse
import threading
import time

DEFAULT_PORT = 8791
MAX_HEADER = 512          # "<host> <port>\n" -- generous
MAX_REQUEST_LINE = 1100   # spec caps the request URL at 1024 bytes
RECV_CHUNK = 65536
TARGET_TIMEOUT = 20.0

# Same allowlist spirit as claude/proxy/claude_amiga_proxy.py: only accept
# connections from the LAN / loopback / Tailscale CGNAT ranges this lab uses.
ALLOWED_PREFIXES = (
    "192.168.",
    "127.0.0.1",
    "::1",
    "10.",
    "100.64.", "100.75.", "100.88.", "100.94.", "100.95.",
)


def log(msg):
    sys.stderr.write("[%s] %s\n" % (time.strftime("%H:%M:%S"), msg))
    sys.stderr.flush()


def ip_allowed(ip):
    return any(ip.startswith(p) for p in ALLOWED_PREFIXES)


def read_line(conn, maxlen, terminator=b"\n"):
    """Read bytes from conn up to and including terminator, or up to maxlen
    bytes, whichever comes first. Returns the bytes read (terminator
    included) or b"" on EOF/overflow."""
    buf = b""
    while len(buf) < maxlen:
        b = conn.recv(1)
        if not b:
            return b""
        buf += b
        if buf.endswith(terminator):
            return buf
    return b""


def handle_client(conn, addr):
    ip = addr[0]
    if not ip_allowed(ip):
        log("DENY %s (not in allowlist)" % ip)
        conn.close()
        return

    try:
        hdr = read_line(conn, MAX_HEADER, b"\n")
        if not hdr:
            log("%s: no header line" % ip)
            return
        try:
            parts = hdr.decode("ascii", "replace").strip().split(" ")
            target_host = parts[0]
            target_port = int(parts[1])
        except (IndexError, ValueError):
            log("%s: bad header %r" % (ip, hdr))
            return

        reqline = read_line(conn, MAX_REQUEST_LINE, b"\r\n")
        if not reqline:
            log("%s: no gemini request line" % ip)
            return

        log("FWD %s -> %s:%d %r" % (ip, target_host, target_port,
                                    reqline[:120]))

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE   # Gemini capsules are almost all
                                          # self-signed; see the module docstring

        raw = socket.create_connection((target_host, target_port),
                                       timeout=TARGET_TIMEOUT)
        try:
            tls = ctx.wrap_socket(raw, server_hostname=target_host)
        except ssl.SSLError as e:
            log("%s: TLS handshake to %s:%d failed: %s"
                % (ip, target_host, target_port, e))
            raw.close()
            return

        tls.sendall(reqline)

        total = 0
        while True:
            chunk = tls.recv(RECV_CHUNK)
            if not chunk:
                break
            conn.sendall(chunk)
            total += len(chunk)
        tls.close()
        log("  <- %d bytes from %s:%d" % (total, target_host, target_port))
    except (socket.error, OSError) as e:
        log("%s: error: %s" % (ip, e))
    finally:
        conn.close()


def serve(bind, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((bind, port))
    srv.listen(8)
    log("Gemini Amiga proxy listening on %s:%d. Ctrl-C to stop." % (bind, port))
    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr),
                                 daemon=True)
            t.start()
    except KeyboardInterrupt:
        log("shutting down")
    finally:
        srv.close()


def selftest(url):
    """Exercise the same TLS relay logic this bridge uses, without needing
    an Amiga: parse a gemini:// URL, fetch it over real TLS, print the
    status line and the first bit of the body."""
    if not url.startswith("gemini://"):
        print("give a gemini:// URL to --selftest")
        return 1
    rest = url[len("gemini://"):]
    if "/" in rest:
        hostport, path = rest.split("/", 1)
        path = "/" + path
    else:
        hostport, path = rest, "/"
    if ":" in hostport:
        host, port_s = hostport.split(":", 1)
        port = int(port_s)
    else:
        host, port = hostport, 1965

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    raw = socket.create_connection((host, port), timeout=TARGET_TIMEOUT)
    tls = ctx.wrap_socket(raw, server_hostname=host)
    tls.sendall((url + "\r\n").encode("utf-8"))
    data = b""
    while True:
        chunk = tls.recv(RECV_CHUNK)
        if not chunk:
            break
        data += chunk
    tls.close()
    head, _, _ = data.partition(b"\r\n")
    print("status line: %r" % head)
    print("body (first 300 bytes): %r" % data[len(head) + 2:][:300])
    return 0


def main():
    ap = argparse.ArgumentParser(description="Gemini Amiga proxy bridge")
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--selftest", metavar="URL", default=None,
                    help="fetch a gemini:// URL directly (no Amiga needed)")
    args = ap.parse_args()

    if args.selftest:
        return selftest(args.selftest)

    serve(args.bind, args.port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
