#!/usr/bin/env python3
"""
mock_relay.py - a tiny hand-rolled Nostr relay for testing nostr.c.

No external dependencies. It performs a real RFC6455 WebSocket handshake,
decodes the client's masked REQ frame, then sends a couple of unmasked
EVENT text frames and an EOSE, exactly as a real relay would. Used to
demonstrate the read path of the AmigaOS nostr client (built with
-DHOST_NET so the same handshake/framing/parse code runs on the host).

Usage:  python3 mock_relay.py [port]   (default 7000)
"""
import base64, hashlib, socket, struct, sys, time

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 7000


def recv_http_headers(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            break
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    return head.decode("latin1"), rest


def ws_accept(key):
    return base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()


def send_text_frame(conn, text):
    payload = text.encode("utf-8")
    n = len(payload)
    hdr = bytearray()
    hdr.append(0x81)  # FIN + text
    if n < 126:
        hdr.append(n)          # server frames are NOT masked
    elif n < 65536:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    conn.sendall(bytes(hdr) + payload)


def read_client_frame(conn):
    b = conn.recv(2)
    if len(b) < 2:
        return None
    opcode = b[0] & 0x0F
    masked = b[1] & 0x80
    ln = b[1] & 0x7F
    if ln == 126:
        ln = struct.unpack(">H", conn.recv(2))[0]
    elif ln == 127:
        ln = struct.unpack(">Q", conn.recv(8))[0]
    mask = conn.recv(4) if masked else b"\x00\x00\x00\x00"
    data = b""
    while len(data) < ln:
        data += conn.recv(ln - len(data))
    out = bytes(data[i] ^ mask[i & 3] for i in range(ln))
    return opcode, out


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)
    print("mock relay listening on ws://127.0.0.1:%d/" % PORT, flush=True)

    conn, addr = srv.accept()
    print("client connected from %s" % (addr,), flush=True)

    head, _leftover = recv_http_headers(conn)
    key = None
    for line in head.split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    print("handshake key: %s" % key, flush=True)
    resp = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n" % ws_accept(key)
    )
    conn.sendall(resp.encode())
    print("sent 101, accept=%s" % ws_accept(key), flush=True)

    frame = read_client_frame(conn)
    if frame:
        print("client REQ frame (opcode %d): %s" % (frame[0], frame[1].decode()),
              flush=True)

    # two example notes (kind 1) then end-of-stored-events
    events = [
        '["EVENT","sub1",{"id":"aa01","pubkey":"npub_deadbeef_author_one",'
        '"created_at":1700000000,"kind":1,"tags":[],'
        '"content":"Hello from a real WebSocket relay to the Amiga! \\u2665"}]',
        '["EVENT","sub1",{"id":"aa02","pubkey":"npub_cafe_author_two",'
        '"created_at":1700000600,"kind":1,"tags":[["e","aa01"]],'
        '"content":"Second note.\\nWith a newline and a \\"quote\\"."}]',
    ]
    for e in events:
        send_text_frame(conn, e)
        time.sleep(0.05)
    send_text_frame(conn, '["EOSE","sub1"]')
    print("sent 2 EVENTs + EOSE", flush=True)

    # give the client a moment, then read its CLOSE and hang up
    time.sleep(0.3)
    try:
        conn.settimeout(0.5)
        f = read_client_frame(conn)
        if f:
            print("client sent (opcode %d): %s" % (f[0], f[1].decode(errors='replace')),
                  flush=True)
    except Exception:
        pass
    conn.close()
    srv.close()
    print("mock relay done", flush=True)


if __name__ == "__main__":
    main()
