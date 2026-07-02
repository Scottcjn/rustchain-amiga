# amiports - a MacPorts-style ports system for classic AmigaOS

Status: MVP WORKING, verified in-guest 2026-07-02. `amiport install`
runs INSIDE the emulated Amiga against an HTTP repo served from the
host: index fetched, archive downloaded, SHA-1 verified, extracted to
SYS:amiports/, registered, and the installed binary executed - all in
one unattended boot (evidence: `test/EVIDENCE_guest_log.txt`).

Working name "amiports" (an AmigaPorts org exists on GitHub; the final
name is Scott's call).

## Vision: the bootstrap ladder

MacPorts made old Macs useful by making software installable again.
amiports does the same for the Amiga, with one doctrine at its core,
straight from the RustChain Amiga spec:

**Native vbcc is rung 1; ports formalize backporting newer compilers
rung by rung.** Every rung of the ladder is a port: the native vbcc
toolchain (devkit/) gets packaged as a port, then software built with it
becomes ports, then a better compiler built with *that* becomes a port,
and so on. Instead of heroic one-off backports that rot in forum
threads, each rung is a Portfile: reproducible, SHA-1 pinned, licensed,
one `amiport install` away. The cross-docker build kind is the crane
that places the first rungs from the host; the ladder's goal is for
later rungs to be built natively on-Amiga.

## What's here

    ports/
      README.md            this file
      FORMAT.md            Portfile / .apak / index / installed.db formats
      tree/                the ports tree (one dir per port)
        hello-amiports/    source-built proof port (hello.c + Portfile)
        rustchain-tools/   prebuilt port: rtcwallet, rtcfetch, rtctop, miner
      harness/
        amiport-build.py   host-side build harness (python3, stdlib only)
      repo/                the built repo (index.txt + packages/*.apak)
      client/
        amiport.c          the on-Amiga client (C89, m68k hunk binary)
        vendor/            vendored rtc_common (origin noted in the files)
        Makefile           cross + host + ILP32 builds
      test/
        run_test.sh        in-guest integration test (see below)
        amiport-test.fs-uae  FS-UAE config for the test
        shared/            the test guest's SYS: volume
        EVIDENCE_*.txt     captured proof from the passing run

## Usage

Host side - build the repo and serve it:

    cd ~/rustchain-amiga/ports
    python3 harness/amiport-build.py build-all
    python3 harness/amiport-build.py serve        # http.server on 0.0.0.0:8873

On the Amiga (or in FS-UAE with `bsdsocket_library = 1`):

    amiport list                       ; show the repo
    amiport info hello-amiports        ; one package's details
    amiport install hello-amiports    ; fetch, verify sha1, extract, register
    amiport installed                  ; local package db

Options: `--repo http://host:port` (default `http://127.0.0.1:8873`,
which reaches the host's loopback from FS-UAE because bsdsocket guest
calls execute as host socket calls - verified empirically, see
`test/EVIDENCE_httpd_access.txt`), `--prefix DIR` (default
`SYS:amiports`). On real hardware over a LAN, pass the host's LAN IP.

Exit codes: 0 ok, 5 usage, 10 network, 12 not in index, 13 sha1
mismatch (nothing extracted), 14 extract/io error.

## How to add a port

1. `mkdir tree/<name>` and write `tree/<name>/Portfile` (see FORMAT.md;
   copy hello-amiports for a source build or rustchain-tools for
   prebuilt artifacts). Record the real `license`.
2. `python3 harness/amiport-build.py build <name>` - compiles (if
   cross-docker), packs the .apak, regenerates `repo/index.txt`.
3. Test it: `python3 harness/amiport-build.py serve` in one shell, then
   either the host client (`make -C client host_amiport &&
   client/host_amiport install <name>`) or the full in-guest run
   (`test/run_test.sh`).

Rules: legal sources only, license recorded per port, no dependencies
on other agents' directories (vendor what you need, note the origin).

## Testing

Three tiers, all exercised for this MVP:

- Host: `make -C client host_amiport` builds the client with real POSIX
  sockets; the full list/install/verify/extract flow runs on Linux
  against the same repo server. Negative paths verified: tampered
  archive -> exit 13 with nothing extracted, unknown package -> 12,
  dead server -> 10, https url -> 5.
- ILP32: `make -C client host32_amiport` (i386/gcc docker) repeats the
  flow with 4-byte longs, the m68k data model, to catch promotion bugs
  x86_64 hides.
- In-guest: `test/run_test.sh` serves the repo, boots FS-UAE headless
  (AROS m68k ROM, see emu/README.md), and the guest's startup-sequence
  runs list -> install hello-amiports -> run the installed binary ->
  install rustchain-tools -> installed. The script verifies every step
  from the log the guest writes. 11/11 checks pass.

## Repo hosting plan

Today: `python3 -m http.server` from `ports/repo/` (dev/test). The repo
is a static directory, so production hosting is a copy:

- Node 1 nginx (50.28.86.131): drop `repo/` under the existing web root
  and serve as plain HTTP on the legacy-miner port pattern; Amigas have
  no TLS, so HTTP is the contract, and SHA-1 verification in the client
  is what makes that tolerable for now.
- GitHub releases later: attach .apak files to releases, keep index.txt
  regenerated by CI; needs an HTTP (not HTTPS) mirror or a tiny proxy
  because github.com is HTTPS-only.
- Nothing gets deployed to prod nodes without Scott's explicit OK.

## Known limits (MVP)

- No dependencies between ports yet (planned: `depends:` key, client
  resolves before install).
- No `amiport remove`/`upgrade` yet; installed.db is append-only, the
  newest line per name wins.
- Archives are uncompressed (see FORMAT.md for why); fine at ~1 MB
  scale, compression is a later rung once a native decompressor port
  exists.
- SHA-1 (not SHA-256) matches the rest of the RustChain Amiga codebase;
  it defends against corruption and casual tampering, not a hostile
  mirror. Signed indexes are future work.
