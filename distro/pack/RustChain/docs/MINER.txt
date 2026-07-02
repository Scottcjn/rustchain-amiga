# RustChain Amiga Miner (m68k)

Single-file C attestation miner for classic AmigaOS 2.0+ (and AROS m68k).
Talks plain HTTP/1.1 to the legacy node endpoint, same protocol as the
mac universal miner v2.2.2.

## Files

| File | Purpose |
|------|---------|
| `rustchain_amiga_miner.c` | The whole miner. One file on purpose. |
| `Makefile` | Cross build via docker + native host smoke test |
| `rustchain_amiga` | AmigaOS hunk executable (build output) |
| `host_test` | Linux smoke-test binary (build output) |

## Toolchain

`bebbo/amiga-gcc` no longer exists on Docker Hub. The working image is the
maintained build of the same toolchain:

```
docker pull amigadev/crosstools:m68k-amigaos
```

That gives `m68k-amigaos-gcc` 6.5.0b with the full NDK including
`proto/bsdsocket.h`, `sys/socket.h`, `netinet/in.h` and `proto/timer.h`.
Nothing else needs installing.

## Build

```
cd ~/rustchain-amiga/miner

# Amiga hunk executable (docker does the cross compile)
make rustchain_amiga
# -> file rustchain_amiga
#    rustchain_amiga: AmigaOS loadseg()ble executable/binary

# Native smoke test of the portable logic (SHA-1, checksum, JSON, HTTP)
make host-test
# -> ALL CHECKS PASSED (0 failures)
```

Exact cross command the Makefile runs:

```
docker run --rm -v $(pwd):/work -w /work amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68000 -O2 -fomit-frame-pointer \
  -Wall -Wextra -Wno-unused-parameter \
  -o rustchain_amiga rustchain_amiga_miner.c
```

`-m68000` keeps the binary runnable on every Amiga; the CPU type is detected
at runtime from ExecBase AttnFlags, not baked in at compile time.

## Usage (on the Amiga)

```
rustchain_amiga --test-only              ; print detection, no network
rustchain_amiga --dry-run                ; print the exact JSON payload
rustchain_amiga --once                   ; attest one time, exit
rustchain_amiga                          ; attest, then every 30 minutes
rustchain_amiga --node 50.28.86.131:8088 --wallet amiga-fsuae-scott
```

Defaults: node `50.28.86.131:8088`, wallet `amiga-fsuae-scott`.
Network needs a running bsdsocket stack (Roadshow/AmiTCP on real hardware,
`bsdsocket_library = 1` in FS-UAE).

## What it detects

- CPU: ExecBase AttnFlags, reported as arch `68000`..`68060` plus FPU
- Raw AttnFlags word as integer (for `tools/validate_amiga.py`)
- Kickstart version: `SysBase->LibNode.lib_Version . SysBase->SoftVer`
- ROM: 512KB window at `0xF80000`
  - SHA-1 (lowercase hex) for the server ROM clustering DB
    (`rom_fingerprint_db.py` format)
  - Unsigned byte-sum checksum, same algorithm the Devpac
    `amiga_fingerprint.asm` intends, comparable against
    `rustchain-poa/tools/rom/checksums.json`
- Free memory via AvailMem
- Timing entropy via timer.device UNIT_ECLOCK (~709 kHz), zeros if the
  timer cannot be opened

Detection reads live hardware only. No AROS assumptions: boot the same
binary on a real Kickstart (Amiga Forever licensed ROMs) and the real
version/hash/checksum get reported.

## Payload format (what the integration test needs to know)

Flow is challenge then submit, matching the mac universal miner:

1. `POST /attest/challenge` body `{}` -> take `nonce` from the JSON reply
2. `POST /attest/submit` with:

```json
{
  "miner": "<wallet>",
  "miner_id": "<wallet>",
  "nonce": "<nonce>",
  "report": {
    "nonce": "<nonce>",
    "commitment": "<sha1 hex>",
    "derived": {"mean_ns":..., "variance_ns":..., "min_ns":..., "max_ns":..., "sample_count":16},
    "entropy_score": <variance_ns>
  },
  "device": {
    "family": "m68k", "arch": "68030", "model": "Amiga (AROS/FS-UAE)",
    "cpu": "MC68030", "fpu": "68882", "cores": 1,
    "memory_gb": 0, "memory_kb": 2048, "machine": "m68k",
    "attn_flags": 55,
    "rom_hash": "<sha1 of 512KB ROM>", "rom_size": 524288,
    "rom_checksum": <unsigned byte sum>,
    "kick_version": "47.96", "miner_version": "1.0"
  },
  "signals": {"macs": [], "hostname": "amiga-fsuae"},
  "fingerprint": {
    "all_passed": false,
    "checks": {
      "clock_drift": {
        "passed": true,
        "data": {
          "mean_ns": 1059625, "stdev_ns": 81930, "cv": 0.077319,
          "drift_stdev": 142080, "timer_source": "eclock",
          "samples_ns": [1000000, 1103000, "...16 raw EClock deltas..."]
        }
      },
      "anti_emulation": {
        "passed": false,
        "data": {"platform": "amigaos", "vm_indicators": ["uae.resource"]}
      }
    }
  }
}
```

Decisions the integration test must know:

- **No Ed25519 signature.** This is the legacy-miner path (same as the
  G4/G5 miners). Emulated hardware is expected to be flagged and get
  minimal weight. That is the point of the test.
- **Fingerprint block is required** since the server started returning
  HTTP 422 MISSING_FINGERPRINT without `fingerprint.checks`. Two honest
  checks are sent, evidence not verdicts:
  - `clock_drift`: 16 raw EClock loop timings (timer.device UNIT_ECLOCK)
    plus mean/stdev/cv/drift_stdev computed in 64-bit integer math. `cv`
    is printed as fixed-point `W.FFFFFF` from cv_ppm, so the server's
    `cv < 0.0001` synthetic-timing check reads it as a normal float.
    `timer_source` is `"none"` and passed=false if timer.device failed.
  - `anti_emulation`: real UAE self-detection. `OpenResource("uae.resource")`
    non-NULL means UAE (all flavors incl. FS-UAE); also scans the
    bsdsocket.library id string for "UAE" (`bsdsocket_id:uae` indicator).
    On the emulator this reports passed=false with the indicator list;
    server responds vm_detected, attestation is ACCEPTED with
    `fingerprint_passed:false` and near-zero weight. On a real Amiga both
    indicators are absent and it reports passed=true.
- **The variance overflow bug is fixed and regression-tested.** v1.0
  briefly shipped 32-bit signed accumulation which overflowed on ~1ms
  EClock deltas (variance_ns = -942169152 in the live FS-UAE run). All
  stats now accumulate in unsigned long long with an integer isqrt for
  stdev; 64-bit values are printed by an in-file u64 decimal helper, no
  reliance on libnix `%lld`. Host test locks mean=1059625,
  variance=6712609375 (does not fit in int32), stdev=81930, cv_ppm=77319,
  drift_stdev=142080 against python-computed references.
- **isqrt64 is shift-free by design (second live-run fix).** The first
  isqrt used 64-bit shifts (`1ULL<<62`, `r>>1`) and bebbo gcc at `-m68000`
  miscompiled them: on target, variance 28133137871 was right but stdev
  came back exactly 2^34 (17179869184), blowing cv up to 2537.87. The
  x86_64 host test could not see it. Current isqrt64 is a binary search
  using only 64-bit add/sub/compare/divide, the exact ops the live run
  proved good on target (variance math and its printed decimal were
  correct). cv_ppm is also unsigned long long now: the bad-stdev quotient
  had silently wrapped the 32-bit `unsigned long` on m68k, so even the
  wrong value was reported wrongly. Regression locks:
  `isqrt64(28133137871) == 167729`, a 16-case table up to u64 max, u64
  printer checked against 17179869184 and 28133137871, and a composed
  pipeline vector at live-run magnitudes (cv 0.263157, not thousands).
- **`commitment` is SHA-1**, not SHA-256. The server stores it as an opaque
  string (`report.get('commitment','')` straight into the tickets table),
  it never recomputes it. Input: `nonce + wallet + arch + rom_hash`.
- **`macs` is an empty list on purpose.** The server's RIP-0147a OUI gate
  412-rejects unknown MAC vendors, and an emulated NIC OUI would trip it.
  Empty list skips the gate.
- **`memory_gb` is integer 0** on small machines (no floats in the payload);
  `memory_kb` carries the real number.
- **`attn_flags` and `rom_checksum`** are for
  `rustchain-poa/tools/validate_amiga.py`, which scores exactly those two
  fields. Verified: our fields plug into `validate_amiga_dump()` unchanged.
  Note the validator penalizes `attn_flags == 0`, but 0 is legitimate on a
  stock 68000 A500. Known validator quirk, reported upstream, the miner
  just reports the raw word.
- Response handling: HTTP status line parsed, then a dumb string scan for
  `"nonce"` / `"ticket_id"` / `"fingerprint_passed"`. Server responses are
  small Content-Length JSON bodies, no chunked handling needed.

Known checksum reference values (from `tools/rom/checksums.json`):
Kick 1.2 = 8321324, 1.3 = 8589954, 2.04 = 8984572, 3.1 = 9432468.
The AROS ROM will NOT match any of these (rom_checksum_mismatch, -400),
which is correct behavior for the emulator test.

## Host test

`make host-test` compiles the same C file with `-DHOST_TEST` (Amiga calls
stubbed) and runs it twice: native x86_64, and 32-bit ILP32 inside the
`i386/gcc` docker image (`make host-test32`). The 32-bit run has
`sizeof(long) == 4` like the m68k target, which is what catches
int-promotion and 64-bit-op bugs the x86_64 run cannot see (this box has
no gcc multilib, so docker instead of `-m32`; same effect, no system
changes). Checks:

- SHA-1 against NIST vectors (empty, "abc", quick brown fox)
- SHA-1 + byte-sum of a synthetic 512KB ROM against precomputed values
- 64-bit entropy stats on samples that would overflow int32 (the live-run
  variance bug), against python-computed references
- isqrt64 16-case table (0, perfect squares, off-by-one, 2^34,
  28133137871 from the live run, 2^62, (2^32-1)^2, u64 max)
- u64 decimal printer against 17179869184, 28133137871, u64 max
- Composed pipeline vector at live-run magnitudes (stdev 250000,
  cv 0.263157)
- Fingerprint block in both variants: UAE detected (emulator run) and
  clean (real hardware run)
- JSON payload builds, contains every required field, braces balance,
  no negative numbers, parses with `python3 -c 'json.loads(...)'`
- JSON string escaping (quotes, backslash, control chars)
- HTTP POST formatting (request line, Host header, Content-Length matches)
- HTTP response parsing (status extraction, nonce extraction)
