# Gemini on the Amiga

A native C client for the Gemini protocol, for classic AmigaOS (m68k) and
AROS. Gemini is the small, deliberately plain "Gopher meets HTTPS" protocol:
a request is one line, a response is a status line plus a body, and the
main content type (`text/gemini`) is a handful of line-prefix rules instead
of HTML. That plainness is what makes it a reasonable thing to bring to a
68k Amiga: no HTML parser, no JavaScript, no images to decode inline, just
text and a link list.

## What it does

```
gemini gemini://geminiprotocol.net/
```

opens a TLS connection to the host (port 1965 by default), sends the
request line `gemini://geminiprotocol.net/\r\n`, reads the response, and:

- on `1x INPUT`, prints the prompt from the server and asks for a line of
  text, then re-requests the same path with that text as a percent-encoded
  query string,
- on `2x SUCCESS` with `text/gemini`, renders it: `#`/`##`/`###` headings,
  `=> url label` links (numbered, so you can see an index and pick one),
  `* ` list items, `>` quote lines, and `` ``` `` toggles a preformatted
  block whose contents are never reinterpreted as headings/links/lists,
- on `2x SUCCESS` with any other `text/*`, prints the body as-is,
- on `2x SUCCESS` with a non-text type, reports the size and MIME type and
  tells you to pass `-o FILE` to save it,
- on `3x REDIRECT`, follows it (up to 5 hops) and shows where it went,
- on `4x`/`5x`/`6x` (temporary/permanent failure, client cert required),
  prints the status code and the server's message.

After a page is shown, it drops to a `gemini>` prompt: type a link number,
a `gemini://` URL, or a relative reference to go there, or leave it blank
(or type `q`) to quit. This makes it a small interactive browser, not just
a one-shot fetcher, but it still behaves fine as a one-shot fetcher: pipe
`/dev/null` at it, or pass `-1`/`--no-interactive`, and it fetches once,
prints, and exits.

## Two transports

Same shape as `claude/client/claude.c` in this repo, for the same reason:

1. **AmiSSL direct TLS (primary, self-contained).** The Amiga does the TLS
   itself with AmiSSL and talks straight to the target capsule. Needs a
   full AmigaOS/AROS with AmiSSL and bsdsocket installed.
2. **Host proxy (fallback, `--proxy host:port`).** For a machine with no TLS
   library (a bare-ROM AROS boot, an AmiTCP-only setup), or just to test
   this client without setting up AmiSSL in an emulator. `proxy/
   gemini_amiga_proxy.py` is a small bridge: the Amiga opens a plain TCP
   connection to it, sends the target host/port on one line and the exact
   Gemini request line on the next, and the bridge does the TLS to the real
   capsule and relays the raw response back byte for byte. Because it is a
   dumb byte relay (not a protocol translator), the Amiga's response parser
   runs identically whether the bytes came from AmiSSL directly or through
   the bridge.

```
python3 proxy/gemini_amiga_proxy.py --port 8791
# on the Amiga (or in FS-UAE):
gemini --proxy 192.168.0.X:8791 gemini://geminiprotocol.net/
```

## TLS trust: no TOFU, be aware of this

Gemini capsules are almost all self-signed -- the spec expects clients to
do TOFU (trust-on-first-use certificate pinning: remember the fingerprint
you saw the first time, and warn if it ever changes), not CA validation.
This client does **not** implement a TOFU store. By default it accepts
whatever certificate the server presents (`SSL_VERIFY_NONE`), which is the
only way most real capsules work at all. Pass `--strict-tls` to require a
CA-validated certificate instead; that will fail against nearly every real
Gemini capsule, which is expected, not a bug. A real TOFU store (a small
`ENVARC:.gemini/known_hosts`-style file of host -> cert fingerprint) would
be a reasonable follow-up; it is not built here.

## Build

```
cd gemini
make host-test     # native + i386 ILP32 self-test of the pure logic
make bin/gemini     # Amiga hunk exe via docker (amigadev/crosstools) + AmiSSL
make bin/gemini-proxy   # Amiga hunk exe, no AmiSSL linked (--proxy only, smaller)
```

`make bin/gemini` cross-compiles with the `amigadev/crosstools:m68k-amigaos`
docker image and links against the AmiSSL SDK that ships in this repo
(`python/src/amissl-sdk/`). `file bin/gemini` reports `AmigaOS loadseg()ble
executable/binary`.

## Layout

```
gemini/
  README.md                    this file
  gemini.c                     the client (one file; C89, m68k rules)
  Makefile                     docker cross build + host self-test
  vendor/
    rtc_common.c/.h             vendored from claude/client/vendor/ (this
                                 client only uses rtc_net_open/rtc_net_close/
                                 rtc_check_break from it -- its URL parser,
                                 status-line parser, and gemtext renderer
                                 are its own, and are pure C89 compiled
                                 unconditionally so the host test needs no
                                 network and no Amiga headers)
  proxy/
    gemini_amiga_proxy.py        host-side TLS bridge (fallback transport)
  bin/
    gemini                       built Amiga hunk executable (AmiSSL-linked)
    gemini-proxy                 built Amiga hunk executable (no AmiSSL)
```

## What the host self-test covers

`make host-test` builds and runs the pure logic natively and again at
`-m32` (ILP32, closer to the Amiga's 32-bit-int world), no network and no
Amiga headers involved. 62 checks:

- **URL parsing** (`gem_parse_url`): scheme with/without a port, default
  path `/`, a query string with no path getting a leading `/`, a bare
  `host/path` with no `gemini://` prefix, `https://` correctly rejected,
  `gemini://` with no host rejected, a `#fragment` stripped from the path,
  and `gem_build_url` round-tripping (omitting the port when it is the
  default 1965, keeping it otherwise).
- **Status-line parsing** (`gem_status_code`/`gem_status_meta`/`gem_body`):
  a normal `20 text/gemini\r\n...` response, a header-only `51 not found`
  response (empty body), a non-digit status code rejected, a missing
  mandatory space after the code rejected, and a lenient bare-`\n` fallback
  for servers that do not send strict `\r\n`.
- **Percent encode/decode round trip**, for the text an `INPUT` response
  sends back as a query string.
- **Relative URL resolution** (`gem_resolve_url`): a plain relative link
  merging into the base page's directory, an absolute path replacing the
  whole path, a `..` climbing out of a directory, an absolute `gemini://`
  link overriding the base entirely, a scheme-relative `//host/path` link,
  a query-only `?q=1` link keeping the existing path, and a non-Gemini
  scheme (`https://`) correctly reported as unresolvable.
- **`text/gemini` rendering** (`gem_render_gemtext`): a fixture with `#`/
  `##`/`###` headings, a labeled link, a link with no label (falls back to
  showing the URL), an external non-Gemini link, `* ` list items, a `>`
  quote line, and a `` ``` `` preformatted block containing a line that
  looks like a list item -- checked to come out **verbatim**, proving the
  preformat toggle actually suppresses reclassification inside a code
  block, which is the one gemtext rule that is easy to get subtly wrong.

All 62 checks pass, natively and at `-m32`.

## Beyond the host test: verified against a real capsule

The host test uses a synthetic fixture. To sanity check the renderer against
real content, not just an invented one, `gemini_amiga_proxy.py`'s own
`--selftest` fetched the live homepage at `geminiprotocol.net` over real
TLS, and a throwaway driver ran that real response body through
`gem_render_gemtext` directly. It rendered headings, a labeled internal
link, an external (non-gemini-scheme) YouTube link, and plain body text all
correctly, with a 7-entry link index. Separately, the exact wire protocol
`proxy_fetch` uses (a plain-TCP client sending `"<host> <port>\n"` then the
Gemini request line, reading the raw relay back) was driven against a
*running* instance of the proxy bridge talking to that same live capsule,
end to end, and it worked. That throwaway driver code was not kept; it is
not a deliverable, and is not the same as the host-test suite, which is
what `make host-test` actually runs and is the durable, repeatable check.

## Honest limitations

- **No in-guest run yet.** The Amiga binary was cross-compiled and verified
  as a real `AmigaOS loadseg()ble executable/binary` (`file bin/gemini`),
  and its logic was verified two other ways (the host test, and against a
  live capsule via the proxy protocol on the host side, both above). It has
  **not** been booted and run inside FS-UAE or on real hardware, so the
  actual in-guest AmiSSL TLS path (bsdsocket + AmiSSL library opens,
  `SSL_connect`, `SSL_read`/`SSL_write` against a real socket under AROS or
  Kickstart) is unverified end to end. The proxy path's protocol logic is
  solid (see above), but it likewise has not been exercised from inside a
  booted Amiga guest, only from a host-side Python client speaking the same
  wire format. `claude/client/test/` has the FS-UAE harness this project
  would want to reuse for that; it was not run here.
- **No TOFU certificate pinning**, as covered above -- TLS trust is
  currently all-or-nothing (`--strict-tls` or nothing) rather than
  per-capsule pinned.
- **Binary/non-text bodies can be saved (`-o FILE`) but not viewed.** The
  save path uses the actual byte count from the socket, not `strlen()`, so
  it is correct even if the body contains embedded NUL bytes; there's just
  no image/audio viewer here, only save-to-disk.
- **No back button / history stack.** Following a link or a redirect
  replaces the current page; there's no way to return to a previous page
  except re-entering its URL. A small history stack would be a reasonable
  follow-up.
- **No client certificates.** Status `6x CLIENT CERTIFICATE REQUIRED` is
  reported like any other failure; there's no support for presenting one
  (relevant to some capsules that gate write access this way).
