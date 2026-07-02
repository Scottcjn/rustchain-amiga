# RustChain Amiga Tools (m68k)

First wave of RustChain CLI tools for classic AmigaOS 2.0+ (and AROS
m68k): a read-only wallet client, a minimal HTTP fetcher, and a network
status monitor. Phase 2 of the RustChain Amiga Edition (see ../SPEC.md).

Built on the same hard-won m68k rules as ../miner/ (no 64-bit shifts,
no %lld, bsdsocket.library, plain HTTP). These tools avoid 64-bit math
entirely: JSON numbers are handled as raw text.

## Layout

| Path | Purpose |
|------|---------|
| `common/rtc_common.[ch]` | Shared code copied from the miner (SHA-1, JSON scans, HTTP GET over bsdsocket, Ctrl-C handling). A later pass swaps this for sdk/ librustchain. |
| `src/rtcwallet.c` | Wallet client |
| `src/rtcfetch.c` | HTTP fetch utility |
| `src/rtctop.c` | Network status |
| `bin/` | Amiga hunk executables (build output) |
| `testdata/` | Real node responses captured with curl, used by the host tests |
| `Makefile` | Docker cross build + host tests |

## Build

```
cd ~/rustchain-amiga/tools

make all           # Amiga hunk binaries into bin/ (docker amigadev/crosstools)
make host-test     # native tests of JSON parsing / formatting vs testdata/
make host-test32   # same tests in a 32-bit ILP32 userland (long = 4 bytes,
                   # like m68k; catches what x86_64 cannot see)
```

All three targets are warning-clean with -Wall -Wextra. `file bin/*`
must say `AmigaOS loadseg()ble executable/binary`.

## Node endpoints used (probed live on 50.28.86.131:8088, 2026-07-02)

| Endpoint | Used by | Notes |
|----------|---------|-------|
| `GET /epoch` | rtcwallet, rtctop | epoch, slot, enrolled_miners, epoch_pot, total_supply_rtc |
| `GET /wallet/balance?miner_id=X` | rtcwallet | amount_rtc / amount_i64; unknown miners return 0.0, not an error |
| `GET /api/miners` | rtctop | wrapper object `{"miners":[...],"pagination":{...}}`, NOT a bare array; capped at 100 entries, newest attestation first |

Plain `/balance` and `/api/balance` are 404 on the node. Requests go
out as HTTP/1.0 so replies are never chunked.

## rtcwallet

Read-only wallet client. Transfers are out of scope on purpose: the
node's `/wallet/transfer/signed` needs Ed25519 signatures and there is
no Ed25519 on the Amiga yet. Use the desktop wallet to send.

```
rtcwallet balance dual-g4-125
  miner:   dual-g4-125
  balance: 718.514429 RTC

rtcwallet epoch
  RustChain epoch info (50.28.86.131:8088)
    epoch:            211
    slot:             30487
    blocks per epoch: 144
    enrolled miners:  23
    epoch pot:        1.5 RTC
    total supply:     8388608 RTC

rtcwallet address
  machine miner id: amiga-68030-aabd0895
```

`address` derives `amiga-<cpu>-<first 8 hex of ROM SHA-1>` from live
hardware (ExecBase AttnFlags + the 512KB ROM at 0xF80000), the same
detection the miner does. It is a suggested per-machine id convention,
not a funded wallet.

`--node host:port` before the command targets another node.

## rtcfetch

The Amiga's curl-lite for RustChain endpoints. http:// only (no TLS on
a stock Amiga stack).

```
rtcfetch http://50.28.86.131:8088/epoch
rtcfetch http://50.28.86.131:8088/api/miners miners.json
rtcfetch -m 256 http://50.28.86.131:8088/api/miners
```

Body goes to stdout (or the outfile); the `HTTP <status>, <n> bytes`
summary goes to stderr so pipes stay clean. Default cap 64 KB, `-m`
raises it up to 1024. Redirects are not followed; the Location header
is printed so you can rerun by hand.

## rtctop

One-shot network status table, fits a 77-column stock AmigaShell:

```
rtctop
  RustChain network status  (50.28.86.131:8088)  epoch 211  slot 30488

  MINER                                     ARCH           MULT  SEEN
  ----------------------------------------- ------------ ------ -----
  RTC41e11e938fc3cb4f77060cca50b89289599... M3              1.1    1m
  modern-sophia-Pow-9862e3be                broadwell      1.05    1m
  ...

  20 shown, 23 enrolled | epoch 211 | supply 8388608 RTC
```

`rtctop -l` refreshes every 60 seconds until Ctrl-C (SIGBREAKF_CTRL_C
is checked during the sleep and during long receives, so it stops
promptly).

Attestation ages are derived from the server's slot number
(genesis + slot*600), so they are correct even when the Amiga clock is
unset. Resolution is one 10-minute slot.

## Exit codes (all tools)

| Code | Meaning |
|------|---------|
| 0 | ok |
| 5 | usage error |
| 10 | network failure (no stack, connect/resolve/send failed) |
| 15 | server answered but not usably (non-200, non-HTTP, missing fields) |
| 20 | host test failure (host builds only) |

## Host tests

`-DHOST_TEST` swaps the Amiga platform layer for stubs and compiles a
test main into each tool. Tests parse the REAL captured responses in
`testdata/` (miners.json, epoch.json, balance.json, epoch_raw.http),
plus SHA-1 NIST vectors, url parsing, truncation, 77-column width
checks and clean-failure paths. `make testdata` re-captures from the
node; the tests lock exact values from the captures, so update the
constants in the host-test sections after refreshing.

## Requirements on the Amiga

- AmigaOS 2.0+ or AROS m68k, any CPU (compiled -m68000)
- A bsdsocket TCP/IP stack for network commands: Roadshow or AmiTCP on
  real hardware, `bsdsocket_library = 1` in FS-UAE
- `rtcwallet address` works without any network stack
