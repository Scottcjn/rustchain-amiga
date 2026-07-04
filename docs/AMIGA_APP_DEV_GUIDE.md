# Building modern apps for a 68k Amiga

A field guide for adding a new native app to classic AmigaOS (m68k) in this
repo, written for developers and for AI coding agents. It captures the traps
that cost real hours so you do not rediscover them. Everything here is verified
against FS-UAE (AROS m68k ROM) and, for the TLS and Kickstart parts, against
real Workbench 3.1.

## The toolchain

One Docker image cross-compiles everything: `amigadev/crosstools:m68k-amigaos`
(bebbo gcc 6.5.0b). A typical build:

```
docker run --rm -v /home/scott/rustchain-amiga:/work -w /work/YOURAPP \
  amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer -Wall \
  -o bin/yourapp yourapp.c
```

`file bin/yourapp` must print `AmigaOS loadseg()ble executable/binary`.

## The five traps (each of these cost hours)

1. **Compile with `-m68020`, not `-m68000`.** bebbo gcc miscompiles the
   `__mulsi3` helper that the 68000 uses for any runtime-variable 32-bit
   multiply. That includes `atoi`, `atol`, `strtol`, `strtoul` (their digit
   accumulation multiplies by a variable base), so those functions HANG. The
   68020 has a native 32-bit multiply, so `-m68020` emits it and dodges the
   whole class. If you must target the plain 68000, replace variable multiplies
   with constants/shifts and hand-roll your integer parsers.

2. **Do not set a libnix `unsigned long __stack` global.** It swaps the program
   onto a heap-allocated stack, and `bsdsocket.library` allocates a per-task
   signal that cannot be freed from that context, so `CloseLibrary(SocketBase)`
   HANGS at exit and the shell never returns. Instead keep large locals
   `static` (they go in BSS, not on the stack), so the default stack suffices.
   The old `const char stack_size[] = "$STACK:..."` cookie is a no-op under
   libnix anyway.

3. **Flush redirected stdout.** When stdout is redirected to a file (`>SYS:x`),
   libnix fully buffers it. If the program hangs or exits uncleanly before the
   flush, the output is lost. Call `fflush(stdout)` right after you print any
   result you care about.

4. **Open `bsdsocket.library` version 3, not 4.** On AROS/FS-UAE, opening v4
   works for socket I/O but its `CloseLibrary` hangs at process exit (and
   leaking it poisons the next same-shell invocation). v3 opens and closes
   cleanly. In FS-UAE, `bsdsocket_library = 1` provides the library; the guest
   reaches host services at `127.0.0.1` (bsdsocket runs the guest's socket calls
   as host socket calls, so guest loopback is the host loopback).

5. **AmigaDOS is not bash.** The command-line length limit is about 512 bytes
   (a long argument gives `Command too long`). `*` is the escape character
   inside quoted strings (so `"***"` breaks parsing). Redirection is `>` and
   `>>` only; there is no `2>>`. A script file is only runnable by name if its
   script protection bit is set; otherwise call it with `Execute file`.

## Networking and TLS

- Plain sockets: use bsdsocket (`socket`/`connect`/`send`/`recv`/`CloseSocket`).
  A proven recv-until-EOF loop and the `rtc_net_open`/`rtc_net_close` helpers
  are in `claude/client/vendor/rtc_common.c` and `tools/common/rtc_common.c`.
- TLS: link AmiSSL. Include path `python/src/amissl-sdk/AmiSSL/Developer/include`,
  lib path `python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3`, link
  `-lamisslstubs`. The full init/teardown sequence (amisslmaster, amissl,
  utility, `InitAmiSSL` with `AmiSSL_SocketBase`) and SSL read/write are in
  `claude/client/claude.c` (`amissl_init`, `amissl_cleanup`, `amissl_post`).
  Reuse them. For a build with no TLS at all, compile that transport out with a
  `-DNO_AMISSL` guard (see the same file) so the binary links without the stubs.
- Fallback pattern: give your app a `--proxy host:port` mode that talks plain
  HTTP/TCP to a small host-side bridge which does the TLS. This makes the app
  testable end to end without depending on in-guest AmiSSL, and keeps any API
  key on the host, never on the Amiga. `claude/proxy/` is a working example.

## JSON without a library

`claude/client/claude.c` has small, dependency-free JSON helpers you can copy:
`json_escape`, `json_unescape`, `find_key`, `get_json_string`, `get_raw_span`,
and `rtc_json_next_obj` (iterate the objects in an array). They are C89 and
host-testable.

## Test on the host first

Guard your pure logic (parsing, protocol framing, hashing, rendering) behind
`-DHOST_TEST` and build a native `cc` binary that exercises it with fixtures.
This is how every app in the repo validates its logic without booting an
emulator. See any `*/Makefile` `host-test` target and the `#ifdef HOST_TEST`
block at the bottom of `claude/client/claude.c`.

## Compiling ON the Amiga (vbcc)

If your app compiles C in-guest (like the Claude tool-use loop driving vbcc),
the boot volume must contain the AmigaDOS system directories `c/`, `devs/`,
`libs/`, `s/`. Without them `vasmm68k_mot`/`vlink` fail silently (no object
file, no error). Copy a full working devkit volume; do not assemble a minimal
one. The proven vbcc pipeline is `vbccm68k` (C to asm), `vasmm68k_mot` (asm to
hunk object), `vlink` (link); put `FailAt 21` at the top of any build script so
a tool's warning return does not abort it. See `devkit/`.

## Debugging a target that hangs or crashes

Normal `printf` output dies in the buffer on a crash. Trace with side-effecting
checkpoints instead: write a marker to a file with `Open`/`Write`/`Close` (or a
flushed `printf`) at each step, so you can see exactly how far execution got.
Keep a known-good sibling program to diff against; that is how the `__stack` and
missing-system-dir bugs were finally cornered.

## House style

C89-friendly (declarations first, no `//` comments, no C99 loop decls). Match
the density and idiom of the surrounding code. READMEs and commit messages in
plain voice, no em-dashes. Commits are DCO-signed (`git commit -s`).
