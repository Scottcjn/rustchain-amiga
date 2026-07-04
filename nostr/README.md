# nostr - a native Nostr client for classic AmigaOS (m68k)

This is a small command line Nostr client that runs on real 68k AmigaOS. It
connects to a Nostr relay over a WebSocket, subscribes for notes, and prints
each note's text and author. It is written in plain C, cross compiled to an
AmigaOS hunk executable, and it reuses the socket and AmiSSL patterns from the
sibling `claude/` client in this repo.

## What works, stated plainly

READ ONLY works. The client does all of this itself, on the Amiga:

1. Opens a TCP socket with `bsdsocket.library`, and for `wss://` relays wraps
   it in TLS using AmiSSL (the same init sequence as `claude/client/claude.c`).
2. Performs the RFC6455 WebSocket client handshake: sends the HTTP
   `Upgrade: websocket` request with a random `Sec-WebSocket-Key`, and verifies
   the relay's `Sec-WebSocket-Accept` against the value it computes itself
   (base64 of sha1 of the key plus the RFC6455 GUID).
3. Speaks RFC6455 framing. Frames it sends are masked with a 4 byte key, as the
   spec requires of clients. Frames it receives are read unmasked. It answers
   ping frames with pong, handles close frames, and joins fragmented messages.
4. Sends a Nostr `REQ` (`["REQ","sub1",{"kinds":[1],"limit":N}]`), then reads
   incoming `["EVENT","sub1",{...}]` messages, parses each event's JSON, and
   prints the author pubkey and the note content. `["EOSE",...]` is handled,
   and `NOTICE` / `CLOSED` are recognized.

PUBLISH is NOT implemented, and this client does not fake it. Publishing a note
requires signing the event with a secp256k1 Schnorr signature (BIP340) over the
event id. A full secp256k1 in C on 68k is a large piece of work, so it is left
as a clearly marked TODO rather than stubbed with a fake signature.

What IS implemented and tested is the easy, testable half of publishing: the
NIP-01 event serialization and the sha256 event id. You can see it with:

```
nostr --publish "hello from the Amiga" --pubkey <64-hex-xonly-pubkey>
```

That prints the exact `[0,pubkey,created_at,kind,tags,content]` serialization
and the sha256 event id, and then a TODO explaining that the remaining step is
the Schnorr signature. The signing hook is the only missing piece; the bytes it
would sign are already produced and verified against an independent tool (see
Testing).

## Building

The Amiga build cross compiles with the `amigadev/crosstools:m68k-amigaos`
docker image and links against the AmiSSL SDK that ships in this repo.

```
make            # host-test + both Amiga binaries
make bin/nostr        # AmigaOS exe with AmiSSL (wss:// and ws://)
make bin/nostr-proxy  # smaller AmigaOS exe, no AmiSSL (ws:// or --proxy only)
make host-test        # run the pure-logic self tests on the host
make demo             # run the read path against a local mock relay (host)
make clean
```

`file bin/nostr` reports `AmigaOS loadseg()ble executable/binary`.

### Why -m68020 and not -m68000

The Makefile uses `-m68020`. This is the same hard won rule the other ports in
this repo follow: bebbo's gcc emits the `__mulsi3` helper for runtime variable
32 bit multiplies at `-m68000`, and that helper hangs on this target (the same
family of bug that breaks `atoi` and 64 bit shifts). `-m68020` emits a native
`MULU.L`. The client also targets a full AmigaOS or AROS with AmiSSL and
bsdsocket, which is 020 class hardware anyway. The hashes in this client use
only shifts, adds and xor, so they avoid runtime multiplies entirely.

## Running on the Amiga

```
nostr ws://relay.example:7000/            plain WebSocket, no TLS
nostr wss://relay.damus.io/               TLS via AmiSSL (needs AmiSSL installed)
nostr --limit 50 --kinds 1 wss://relay/   ask for 50 notes of kind 1
nostr --eose-exit wss://relay/            stop after stored events are sent
nostr --proxy 127.0.0.1:8080 wss://relay/ send plaintext to a host proxy that
                                          terminates TLS, keeping the relay's
                                          Host header
```

Notes for a real Amiga or FS-UAE guest:
- A TCP/IP stack must be running (Roadshow or AmiTCP), or in FS-UAE set
  `bsdsocket_library = 1`. `bsdsocket.library` is opened at version 3.
- For `wss://`, AmiSSL must be installed in the guest. If the CA bundle is not
  set up, `--insecure` skips certificate verification.
- The guest reaches the host machine at `127.0.0.1` when FS-UAE bsdsocket is
  used, which is handy for pointing at a local relay or proxy.

## How this was tested

Two layers of testing, both run on the host.

### 1. Pure logic self tests (`make host-test`)

The cryptography, WebSocket framing, and Nostr parsing are pure functions that
compile for the host too, under `-DHOST_TEST`. They are checked against known
vectors, so a build on any machine can prove the logic is correct without an
Amiga:

- sha256 of `""` and `"abc"` against the standard vectors.
- base64 of `""`, `f`, `fo`, `foo`, `foobar`.
- The RFC6455 worked example: key `dGhlIHNhbXBsZSBub25jZQ==` must produce accept
  `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=` (this exercises sha1 and base64 together).
- WebSocket frame encode then decode round trip for a masked client frame,
  including a check that the payload is actually masked on the wire.
- Decode of a server (unmasked) frame, a short/incomplete frame, and a
  126 extended length frame.
- Nostr parsing of an `["EVENT",...]` message: pull the event object, read the
  pubkey, and unescape the content (quotes and newline).
- `EOSE` tag detection.
- NIP-01 event serialization and the sha256 event id, checked byte for byte
  against a value computed independently with Python's `hashlib` (two vectors,
  one with plain content and one with escaped quotes, newline and tab).

The suite is run both natively and, where the host toolchain supports `-m32`,
as an ILP32 build, so the 32 bit pointer and `long` behavior of the m68k ABI is
exercised too. All checks pass in both.

### 2. Read path against a live mock relay (`make demo`)

To prove the actual wire behavior, the same networking code (the handshake,
framing, message assembly, parsing and printing) is compiled for the host with
`-DHOST_NET`, which swaps only the socket primitives (POSIX sockets for
bsdsocket) and drops AmiSSL. Everything else is the exact code the Amiga binary
runs.

`tools/mock_relay.py` is a tiny hand rolled Nostr relay with no external
dependencies. It does a real RFC6455 handshake, decodes the client's masked
`REQ`, then sends two unmasked `EVENT` text frames and an `EOSE`, just like a
real relay. The host build connects to it and prints the notes. Observed:

```
[nostr] WebSocket open (0 leftover byte(s) buffered)
[nostr] -> ["REQ","sub1",{"kinds":[1],"limit":20}]
----------------------------------------
author : npub_deadbeef_author_one
at     : 1700000000 (kind 1)
Hello from a real WebSocket relay to the Amiga! (heart)
----------------------------------------
author : npub_cafe_author_two
at     : 1700000600 (kind 1)
Second note.
With a newline and a "quote".
---- end of stored events (2 shown) ----
```

The relay side confirms it received a correctly masked REQ frame and that the
client accepted the handshake and later sent a clean `CLOSE`. The heart above is
a UTF-8 character delivered as `♥` in the event JSON, decoded by the
client, which shows the JSON unescape path works end to end.

Note: the host build and mock relay are a test harness only. The shipped
artifacts are `bin/nostr` and `bin/nostr-proxy`, the cross compiled AmigaOS
executables.

## Files

```
nostr.c                 the client (crypto, WebSocket, Nostr, one file)
vendor/rtc_common.c/.h  bsdsocket open/close and Ctrl-C helpers, vendored from
                        the repo's tools (used by the Amiga build only)
tools/mock_relay.py     dependency free mock relay for the read path demo
Makefile                docker cross build + host tests + demo
bin/nostr               AmigaOS hunk exe, AmiSSL (wss:// and ws://)
bin/nostr-proxy         AmigaOS hunk exe, no AmiSSL (ws:// or --proxy)
```

## The publish TODO in detail

Everything up to the signature is done and tested. To finish publishing:

1. Derive the x-only public key from a private key (secp256k1).
2. Build the event and compute its id (already implemented:
   `nostr_serialize` and `nostr_event_id`, both host tested).
3. Sign the 32 byte event id with a BIP340 Schnorr signature (secp256k1).
4. Send `["EVENT",{...,"id":"<id>","sig":"<64 byte hex>"}]` over the same
   WebSocket, and read the relay's `["OK",<id>,true|false,...]` reply.

Only step 1 and step 3 are missing, and both need a secp256k1 implementation. A
small pure C secp256k1 with BIP340 could be vendored here and cross compiled the
same way the hashes are, but it is deliberately not faked in the meantime. No
private keys are stored anywhere in this client.
