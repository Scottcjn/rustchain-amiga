# librustchain - RustChain SDK for AmigaOS, Quickstart

librustchain is the working Amiga miner (`../miner/rustchain_amiga_miner.c`,
live-attested against the production node) factored into a static library
you can build RustChain tools against: HTTP over bsdsocket, JSON build and
parse, SHA-1, hardware detection, and the full attestation client.

## Layout

```
sdk/
  include/rustchain/   public headers (rc_http, rc_json, rc_sha1,
                       rc_hw, rc_attest, rc_u64)
  src/                 implementations, extracted from the miner
  lib/librustchain.a   m68k static library (build output)
  examples/            attest_demo.c, hello_rustchain.c
  tests/host_test.c    host smoke test with the miner's regression vectors
  Makefile             docker cross build + host tests
```

## Build everything (one-liner each)

No local toolchain needed, docker does the cross compile. The image is
`amigadev/crosstools:m68k-amigaos` (bebbo gcc 6.5.0b with the full NDK;
`bebbo/amiga-gcc` no longer exists on Docker Hub, this is the maintained
build of it).

```
cd ~/rustchain-amiga/sdk
make lib          # -> lib/librustchain.a
make examples     # -> examples/attest_demo, examples/hello_rustchain (hunk exes)
make host-test    # native + 32-bit smoke tests + python json.loads pass
```

The exact cross command, if you want it without make:

```
docker run --rm -v $(pwd):/work -w /work amigadev/crosstools:m68k-amigaos \
  sh -c 'm68k-amigaos-gcc -noixemul -m68000 -O2 -fomit-frame-pointer \
    -Wall -Wextra -Iinclude -c src/rc_u64.c -o build/rc_u64.o && \
    ... (one -c per src file) ... && \
    m68k-amigaos-ar rcs lib/librustchain.a build/*.o'
```

## Link your own program

```
docker run --rm -v $(pwd):/work -w /work amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68000 -O2 -Iinclude \
    -o mytool mytool.c lib/librustchain.a
```

`-m68000` keeps the binary runnable on every Amiga; the CPU type is
detected at runtime from ExecBase AttnFlags, not baked in at compile time.

Minimal program (this is examples/hello_rustchain.c, shortened):

```c
#include <rustchain/rc_http.h>

int main(void)
{
    static char resp[RC_RESP_MAX];
    if (!rc_http_open()) return 10;
    if (rc_http_get("50.28.86.131", 8088, "/health", resp, sizeof(resp)) > 0)
        printf("HTTP %d\n%s\n", rc_http_status(resp), rc_http_body(resp));
    rc_http_close();
    return 0;
}
```

A full attestation is about a dozen SDK calls, see examples/attest_demo.c:
`rc_hw_detect` + `rc_http_open` + `rc_attest_once` + cleanup.

## API tour

| Header | What you get |
|--------|--------------|
| `rc_http.h` | `rc_http_open/close` (bsdsocket.library), `rc_http_get/post`, request formatters, `rc_http_status/body` |
| `rc_json.h` | `rc_json_escape`, `rc_json_find_string`, and the `rc_jb` bounds-checked buffer builder (objects, arrays, str/int/u64/bool/raw) |
| `rc_sha1.h` | in-file SHA-1, `rc_sha1_hex` one-shot |
| `rc_hw.h` | `rc_hw_detect` (AttnFlags CPU/FPU, Kickstart version, 512KB ROM SHA-1 + byte-sum, free mem), `rc_entropy_collect` (EClock), `rc_emu_detect` (UAE self-detection), `rc_sleep`, `rc_timer_cleanup` |
| `rc_attest.h` | `rc_attest_build_payload` (exact miner wire format), `rc_attest_once` (challenge/submit flow) |
| `rc_u64.h` | `rc_u64s` decimal printer, `rc_isqrt64` shift-free square root |

Everything is C89-friendly: no `//` comments, no mid-block declarations,
no malloc, no floats. It compiles under bebbo gcc and should port to vbcc
with little friction.

Fail-closed rule: the payload builder and the HTTP formatters return -1
if the output would not fit its buffer. Nothing truncated ever goes on
the wire; check the return value.

## The m68k gotcha list (learned the hard way, on the live node)

These are baked into the SDK so you do not rediscover them. If you write
code outside the SDK, respect them:

1. **No 64-bit shifts at -m68000.** bebbo gcc 6.5 miscompiles them:
   `1ULL<<62` and `r>>1` produced a stdev of exactly 2^34 on target while
   the x86 host test passed. 64-bit add, sub, compare and divide are fine
   (proven in the live run). `rc_isqrt64` is shift-free for this reason.
2. **No `printf("%lld")` on libnix.** It is not a safe bet. Print 64-bit
   values with `rc_u64s()`.
3. **All timing stats in `unsigned long long`.** 32-bit accumulation
   overflowed on real ~1ms EClock deltas (variance came out -942169152).
   Watch intermediate quotients too: a 64-bit value divided into an
   `unsigned long` once wrapped silently on m68k.
4. **ILP32: `long` is 4 bytes.** The x86_64 host test cannot catch
   int-promotion bugs; that is what `make host-test32` (i386 docker) is
   for. Run both.
5. **Big endian.** `htons` is identity on m68k; do not double-swap.
   Multi-byte wire data must be built byte-wise (see rc_sha1.c).
6. **No floats in payloads.** cv is printed as fixed-point `W.FFFFFF`
   from parts-per-million integers; `memory_gb` is integer 0 and
   `memory_kb` carries the real number.
7. **Small default stack.** AmigaOS shells default to 4KB. Large buffers
   in the SDK are `static`, not stack locals. Do the same in your tools,
   or require `stack 16384` in scripts.
8. **`-noixemul`** links against libnix instead of the ixemul.library
   Unix emulation, so binaries run on a stock machine with no extra
   libraries installed.
9. **Emulation honesty.** `OpenResource("uae.resource")` is non-NULL on
   every UAE flavor including FS-UAE. The SDK reports it truthfully and
   the server flags emulated miners to near-zero weight. That is by
   design; do not try to hide it.

## Testing your code without an Amiga

Compile with `-DHOST_TEST` and the Amiga-only calls become deterministic
stubs (fixed 68030, synthetic ROM, fixed entropy samples), the same
values the shipped miner's regression suite locks:

```
gcc -DHOST_TEST -O2 -Wall -Wextra -Iinclude -o mytest mytest.c src/*.c
```

`tests/host_test.c` shows the pattern and carries every vector from the
miner: SHA-1 NIST vectors, `rc_isqrt64(28133137871) == 167729` (the 2^34
bug), the 64-bit variance numbers, payload field checks, and a byte-level
guarantee that `rc_attest_build_payload` output parses with python3
`json.loads`.

## Native development on the Amiga itself

Cross-compiling via docker is the fast path, but the SDK sources are
deliberately buildable on-Amiga:

- **vbcc**: the code is C89-clean (no `//`, no mixed declarations, no
  long long *literals suffixed beyond ULL*... vbcc 0.9+ with the m68k
  target and the OS3.x NDK compiles this style). Set up vbcc with the
  AmigaOS NDK 3.2 includes and add `-Iinclude` the same way.
  Note: vbcc's `long long` support needs `-c99`; test the 64-bit math
  paths against the host vectors before trusting a vbcc build.
- **Devpac heritage**: the ROM byte-sum checksum matches what the Devpac
  assembly fingerprint tool in `rustchain-repo/rustchain-poa/tools/amiga`
  (`amiga_fingerprint.asm`) computes, and `attn_flags`/`rom_checksum`
  plug straight into `rustchain-poa/tools/validate_amiga.py`. If you are
  writing pure-asm tools, that directory is the reference.
- **SAS/C or StormC**: untested, but nothing in the public headers uses
  gcc extensions.

On-Amiga you skip docker entirely: copy `include/`, `src/` and a vbcc or
SAS/C makefile onto the machine (or the shared FS-UAE directory) and
build there. The only true build dependencies are the NDK headers and
`bsdsocket.library` at runtime.

## Wire protocol notes

The attest module speaks the legacy plain-HTTP path
(`http://50.28.86.131:8088`, nginx proxies to the node), the same
protocol as the Mac universal miner v2.2.2: `POST /attest/challenge {}`
then `POST /attest/submit` with the payload. No Ed25519 on this path.
The full field-by-field rationale (commitment, empty macs list,
RIP-0147a, fingerprint evidence) lives in `../miner/README.md`; the SDK
host test proves `rc_attest_build_payload` output is byte-identical to
the shipped miner's.
