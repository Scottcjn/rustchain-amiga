# Mastodon on the Amiga

A native C command-line client for the Mastodon / Fediverse REST API, for
classic AmigaOS (m68k) and AROS. Two commands:

```
mastodon timeline              show the home (or public) timeline
mastodon post "text"           post a status, "Tooting from an Amiga."
```

This follows the same shape as `claude/client/claude.c` in this repo (same
m68k rules, same two-transport design), reused because that client already
proved out AmiSSL + bsdsocket on this exact toolchain. `mastodon.c` is a
single self-contained file (no vendored helper file), so the cross build is
one `gcc` command.

## What it does

- `mastodon timeline` calls `GET /api/v1/timelines/home` if it has a token,
  or `GET /api/v1/timelines/public` if it does not. It walks the returned
  JSON array and prints each status: `@acct (Display Name)`, then the
  `content` field with its HTML tags stripped and the common entities
  decoded (`&amp;`, `&lt;`, `&gt;`, `&quot;`, `&#39;`, `&nbsp;`). `<br>` and
  `</p>` become a line break so paragraphs do not run together. Boosts
  (reblogs) are read through to the original toot's account and text and
  marked `[boost]`.
- `mastodon post "text"` calls `POST /api/v1/statuses` with a JSON body
  `{"status":"text"}` and prints the resulting toot's URL and id.

## Auth and where the token lives

The token is read from `ENV:MASTODON_TOKEN`, or failing that from a plain
text file at `SYS:.mastodon/token` (just the token, nothing else). It is
never hardcoded in `mastodon.c` and is not committed anywhere in this repo -
grep the source yourself, the only "token" string in it is the fixture
`testtoken123` used by the host self-test. The instance host comes from
`ENV:MASTODON_HOST` or `--host name`.

In **AmiSSL direct mode** the token goes straight from the Amiga's own
environment/file into the TLS-encrypted request the Amiga itself makes to
the instance. Nothing else ever sees it.

In **proxy mode** (`--proxy host:port`, see below) the Amiga is still the
one deciding what to send: it builds a small JSON envelope containing the
method, path, its own token, and the post body, and sends that in plaintext
to a local bridge process. This is a different trust model from
`claude/client/claude.c`'s proxy (which deliberately keeps its API key on
the proxy host and never sends it to the Amiga at all) - here the credential
is Mastodon-account-specific and the Amiga is the only place it is
configured, so proxy mode is meant for a trusted local bridge on your own
LAN, or (the main use of it here) the `--mock` bridge below, which does not
need a real token at all. Be aware of this if you ever point `--proxy` at
something you do not control.

## Build

```
cd mastodon
make host-test          # native self-test: JSON, HTML-strip, request builders
make host-proxy-demo    # native build of the real client, POSIX sockets, --proxy only
make bin/mastodon       # Amiga m68k hunk exe via docker (amigadev/crosstools) + AmiSSL
```

The exact cross-compile command (also what `make bin/mastodon` runs):

```
docker run --rm -v /home/scott/rustchain-amiga:/work -w /work/mastodon \
  amigadev/crosstools:m68k-amigaos m68k-amigaos-gcc \
  -noixemul -m68020 -O2 -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter \
  -I/work/python/src/amissl-sdk/AmiSSL/Developer/include \
  -o bin/mastodon mastodon.c \
  -L/work/python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3 -lamisslstubs
```

Actually run in this session:

```
$ file bin/mastodon
bin/mastodon: AmigaOS loadseg()ble executable/binary
```

`bin/mastodon` needs a full AmigaOS/AROS with `bsdsocket.library` v4 and, for
direct HTTPS, AmiSSL v5 installed (same requirement as `claude/client/`).

### m68k gotchas this obeys (see mastodon.c's top comment for the full list)

- **`-m68020`, not `-m68000`.** bebbo's gcc miscompiles the variable-multiply
  helper (`__mulsi3`) and 64-bit shifts at `-m68000`, which hangs the binary
  at runtime. This client parses the `--proxy host:port` port number with a
  hand-rolled shift/add parser (`rc_strtoul`/`rc_atol`), not `atoi`/`strtol`,
  for the same reason - it is not safe on this target even at `-m68020`
  unless the multiply is a compile-time constant, which `*10` and `<<4` are.
- No `unsigned long __stack` global (the libnix quirk that hangs
  `CloseLibrary(bsdsocket)` at exit).
- Big buffers (`RESPBUF`, `OBJBUF`, etc.) are `static` (BSS), not stack.
- `fflush(stdout)` after every informational print.
- HTTP responses are de-chunked in place if `Transfer-Encoding: chunked`
  shows up (Mastodon's own compact JSON responses usually will not need
  this, but the API sits behind arbitrary reverse proxies).

## The two transports

1. **AmiSSL direct (primary).** The Amiga does the TLS itself, straight to
   the instance, following the exact `amissl_init`/`SSL_CTX_new`/
   `SSL_connect` sequence from `claude/client/claude.c` (same SDK example,
   same error messages about installing AmiSSL or a CA bundle). Needs
   `bsdsocket.library` v4 and AmiSSL v5.

2. **Plain-HTTP proxy fallback (`--proxy host:port`).** For a machine with
   no TLS library, or for testing without any of that. The Amiga sends the
   plaintext envelope described above to a small Python bridge,
   `proxy/mastodon_amiga_proxy.py`, which either:
   - relays it for real: opens `https://{host}{path}` with the given
     token and method, and hands back the instance's real status and body,
     or
   - (`--mock`) skips the network entirely and returns a canned timeline /
     a canned "created" response that echoes back whatever you posted.

   The bridge's response is a normal HTTP response (status line + headers +
   body); the Amiga client parses it exactly the same way it would parse a
   direct AmiSSL response, so the timeline/post logic does not know or care
   which transport ran.

## Proof this actually works: end to end, no Amiga required

`mastodon.c` is one file compiled three ways. The AmiSSL-direct code and the
AmigaOS-only bits (`Open`/`Read` for the token file, `OpenLibrary` for
`bsdsocket.library`) are the only parts that cannot run off an Amiga. The
proxy transport (`proxy_request`, `build_proxy_envelope`) and the actual
command logic (`do_timeline`, `do_post`, `parse_status`, `html_strip`) are
plain C with BSD sockets, so `-DHOST_PROXY_DEMO` compiles that same code
natively with POSIX sockets instead of `bsdsocket.library`. This is not a
separate reimplementation - it is the identical source that also goes into
`bin/mastodon`, just compiled for `cc` instead of `m68k-amigaos-gcc` and with
the AmiSSL path replaced by a one-line stub that says "use --proxy" if you
try it here.

Actually run in this session:

```
$ python3 proxy/mastodon_amiga_proxy.py --mock --port 8791 &
[22:29:55] Mastodon Amiga bridge listening on 0.0.0.0:8791 (POST /), mock=True.

$ ./host_mastodon_proxydemo timeline --host mock.example --proxy 127.0.0.1:8791
@alice (Alice A.)
Hello from the mock Fediverse! Testing @mastodon on an Amiga.

@carol@retro.example (Carol C)  [boost]
Boosted toot: retro computing rules.
Second line after a break.

@dave (Dave on a 68030)
No AmiSSL needed for this one, it came through the mock bridge.

$ MASTODON_TOKEN=demo-token-not-real MASTODON_HOST=mock.example \
    ./host_mastodon_proxydemo post "Tooting from an Amiga (demo)" --proxy 127.0.0.1:8791
Tooting from an Amiga.
https://mock.example/@amiga/2001
(status id 2001)
```

Bridge log for that run:

```
[22:30:02] MOCK GET host=mock.example path=/api/v1/timelines/public token=no body_len=0
[22:30:02]   <- 200 (903 bytes)
[22:30:08] MOCK POST host=mock.example path=/api/v1/statuses token=yes body_len=41
[22:30:08] MOCK created status: 'Tooting from an Amiga (demo)'
[22:30:08]   <- 200 (226 bytes)
[22:30:08] MOCK GET host=mock.example path=/api/v1/timelines/home token=yes body_len=0
[22:30:08]   <- 200 (903 bytes)
```

Note the timeline call switches from `.../public` to `.../home` the moment a
token is set, exactly as `mastodon.c` is supposed to behave. `demo-token-not-
real` is a made-up string typed at the shell for this demo, not a real
credential, and nothing in this repo remembers it after the shell exits.

To point the bridge at a real instance instead of the mock, drop `--mock`;
it will then relay to `https://{host}{path}` for real using whatever
host/token the Amiga (or `host_mastodon_proxydemo`) sends it.

## Run it for real

### Option A - AmiSSL direct (recommended)

On the Amiga:

```
setenv MASTODON_HOST mastodon.social
setenv MASTODON_TOKEN your-access-token
copy ENV:MASTODON_HOST ENVARC: ; copy ENV:MASTODON_TOKEN ENVARC:   ; to persist

mastodon timeline
mastodon post "Hello from a real 68k Amiga."
```

Or put the token in `SYS:.mastodon/token` instead of `ENV:MASTODON_TOKEN` if
you would rather not have it in the shell environment.

### Option B - host proxy

```
python3 mastodon/proxy/mastodon_amiga_proxy.py --port 8791   # relay mode, real TLS
```

On the Amiga:

```
mastodon timeline --host mastodon.social --proxy 192.168.0.50:8791
```

The bridge does the TLS; the Amiga still supplies its own token in the
envelope it sends, as described above.

## Layout

```
mastodon/
  README.md                    this file
  mastodon.c                   the client, single file, C89, m68k rules
  Makefile                     docker cross build + two native test builds
  bin/mastodon                 built Amiga hunk executable (gitignored)
  proxy/
    mastodon_amiga_proxy.py    host-side bridge: real relay, or --mock
```

## Honest scope

- Two commands only: `timeline` and `post`. No boosting, favoriting,
  notifications, or multi-account switching.
- The HTML stripper is deliberately simple: it strips all tags and decodes
  six common entities, and turns `<br>`/`</p>`/`</blockquote>` into a single
  newline. It does not render links, mentions, or hashtags specially.
- The proxy transport's trust model is different from `claude/client/`'s
  (see "Auth and where the token lives" above) - know that before pointing
  `--proxy` at a bridge you do not control.
- Not tested on real hardware or under FS-UAE in this session - `bin/mastodon`
  is verified as a real AmigaOS hunk executable (`file` says
  `AmigaOS loadseg()ble executable/binary`) and the identical client logic
  is proven end to end over the proxy path natively (see above), but nobody
  has yet booted it on a Workbench/AROS image the way `claude/test/` does
  for the Claude client. That would be the natural next step if this needs
  to be demoed live on a real or emulated Amiga.
