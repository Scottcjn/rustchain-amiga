# Lua 5.4.7 for classic AmigaOS (m68k)

Status: the interpreter and compiler both cross-compile cleanly to real
AmigaOS hunk binaries and link with zero unresolved symbols. The same
patched sources build and run correctly on the host (Linux x86_64),
which is the strongest evidence available in this sandbox that the
AmigaOS-specific changes did not break Lua itself. Booting the
cross-compiled binary inside this repo's FS-UAE + bare AROS ROM test
rig did not complete in the time available - see "On-Amiga execution"
below for exactly what was tried and what is still open. Nothing here
was declared working without being run and checked.

## What this is

Lua 5.4.7 (unmodified language/VM), pulled from `https://www.lua.org/ftp/`,
built two ways from the identical patched source tree:

- **Cross build**: `m68k-amigaos-gcc` (the `amigadev/crosstools:m68k-amigaos`
  Docker image already used by the rest of this repo's `ports/`), producing
  `bin/lua` and `bin/luac`, real AmigaOS loadseg()-able hunk binaries.
- **Host build**: the same source files, same `luaconf.h`, built with the
  host's native `cc`, producing `host/lua`. This exists purely to prove
  that the one functional change made to `luaconf.h` (see below) is still
  correct Lua - if the host build were broken, the cross build would be
  equally suspect no matter what `file` says about it.

## The one number-type decision: `LUA_32BITS`

Stock Lua 5.4's default number config is `long long` (64-bit integer) and
`double` (64-bit float). `luaconf.h` is patched (see `patches/luaconf-amiga.diff`
and the full annotated copy in `patches/luaconf-amiga.h`) to set:

```c
#define LUA_32BITS 1
```

This makes `lua_Integer` a plain 32-bit `int` and `lua_Number` a 32-bit
`float`, instead of 64-bit `long long`/`double`. That is the *only*
functional change to Lua anywhere in this port - no other macro is
touched, no `.c` file is patched.

Why: on m68k with the `-noixemul` libnix C library and no assumed 68881
FPU, every 64-bit integer op needs libgcc's `__divdi3`/`__muldi3`-style
soft routines, and every `double` op needs full 64-bit software float.
This repo's own CLAUDE.md build notes call out `-m68000` miscompiling
`__mulsi3`/`atoi` on this exact toolchain, i.e. this compiler's soft-math
codegen at low CPU settings is a known hazard here, not a hypothetical
one. Sticking to 32-bit `int` (m68k's native register width, real `MULS`/
`DIVS` hardware instructions) and 32-bit `float` (half the software-math
work of `double`) is the more conservative, honest choice for this
target, at the cost of:

- No 64-bit Lua integers (`math.maxinteger` is `2147483647`, not
  `9223372036854775807`).
- `float` precision (~7 significant decimal digits) instead of `double`
  (~15). `math.pi` prints as `3.141593`, not `3.1415926535898`.

Verified on the host build (`host/verify.lua` / `host/verify_output.txt`):
`math.type(6*7)` is `integer`, `math.type(6/7)` is `float`, and
`math.maxinteger`/`math.mininteger` are exactly the 32-bit values, so the
number model is doing what it's supposed to, not silently falling back
to something else.

### The one library gap this uncovered: no `log2f`

`m68k-amigaos-gcc -noixemul -m68020` links against libnix's
`libm020/libm.a`. That archive has every single-precision math function
`lmathlib.c` needs (`sinf`, `cosf`, `logf`, `powf`, `atan2f`, ...) except
`log2f` - its object file (`sf_log2.o`) is present in the archive but
empty (confirmed with `nm`; the double-precision `log2` is there and
correct). `math.log(x, 2)` is the only caller. Fixed with a 6-line shim,
`patches/lua_libm_compat_amiga.c`:

```c
float log2f(float x) { return (float)log2((double)x); }
```

This is the only non-Lua source file in the port. It is compiled into
both the cross and host builds for consistency, even though the host
libm already has a real `log2f`.

## Build

```
./build.sh            # cross + host
./build.sh cross       # AmigaOS binaries only (needs docker)
./build.sh host        # host sanity binary only (needs a native cc)
```

Fetching Lua first (if `src-lua/lua-5.4.7/` isn't there):

```
cd src-lua
curl -sSLO https://www.lua.org/ftp/lua-5.4.7.tar.gz
tar xzf lua-5.4.7.tar.gz
```

`build.sh` copies the pristine sources into a scratch dir, drops in
`patches/luaconf-amiga.h` as `luaconf.h` and `patches/lua_libm_compat_amiga.c`,
then, per file, exactly as specified for this repo's cross toolchain:

```
docker run --rm -v <repo>/lua:/work -w /work/lua \
  amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer -c <file>.c
```

then links all the `.o` files with `-lm` (needed - see below) into
`lua` and `luac`.

**`-m68020`, not `-m68000`**: this repo's own build notes record that
`-m68000` miscompiles `__mulsi3`/`atoi` on this exact bebbo-gcc build,
hanging real code. Lua's VM does a lot of integer arithmetic
(`luaV_execute`'s opcode dispatch), so this is not a theoretical risk
here - `-m68020` gets real `MULS.L`/`DIVS.L` instructions and gives up
running on plain 68000 Amigas in exchange for a toolchain that is known
to compile Lua correctly.

**`-lm` is required, and its position on the command line matters less
than expected**: without it, linking fails - not just on the expected
transcendental functions (`powf`, `log`), but on basic soft-float
arithmetic (`__addsf3`, `__subsf3`, `__mulsf3`, `__gesf2`, ...). On this
target, libnix's `libm.a` is where *all* IEEE-754 single-precision math
lives, arithmetic included, not just `sin`/`cos`/`log`. This was
empirically verified both ways (linking without `-lm` fails on
`__subsf3` etc; linking with it, even with `-lm` positioned *before*
the source files on the command line as `ports/harness/amiport-build.py`
does, succeeds - this toolchain's driver/linker handles that ordering
fine, verified directly, not assumed).

No `__stack` global is declared anywhere in this port (Lua's own stack
is heap-allocated via `lua_newstate`/the allocator, so there is nothing
to declare a fixed size for).

## Verification actually performed

```
$ file bin/lua bin/luac
bin/lua:  AmigaOS loadseg()ble executable/binary
bin/luac: AmigaOS loadseg()ble executable/binary
```

Host build, same sources, same `luaconf.h` (`host/verify.lua`, full
output in `host/verify_output.txt`):

```
6*7 =                            42
type of 6*7:                     integer
type of 6/7:                     float
maxinteger:                      2147483647
sum 1..100 =                     5050
fib(20) =                        6765
upper:                           HELLO, AMIGAOS LUA!
format:                          42-amiga- 3.14
sorted:                          1,2,3,4,5
math.sqrt(2) =                   1.414214
math.log(8,2) =                  3.0
metatable add:                   30
coroutine.resume(...) x4 =       true 2 / true 12 / true 103 / true done 1000
ALL TESTS COMPLETED OK
```

This exercises base, string, table, math, and coroutine library
functions, closures, metatables, and recursion - a real cross-section
of the language, not just `print(6*7)`.

## On-Amiga execution: what was tried, honestly

The task asked to verify with `file(1)` plus a host-build sanity check,
which is done above. Past that, this repo already has a working FS-UAE
+ bare AROS m68k ROM test rig (`emu/`), so it was worth trying to
actually boot the cross-compiled `lua` and run a script, in an isolated
test hard drive (`emu-test/`, not the `shared/` volume the miner
workstream uses).

**Result: inconclusive, not a pass.** Booting `bin/lua` against a test
script under that rig did not print any Lua output within a 60-90s
window before being killed. What was found while chasing it, in order:

1. First attempt (plain `-m68020` build): AROS printed
   `locale.library failed to load`, `mathieeedoubbas.library failed to
   load`, `mathieeedoubtrans.library failed to load`, `mathieeesingtrans.library
   failed to load`, then hung. `nm` on libnix's `libm020/libm.a` confirms
   why: every float op resolves to a call through `_MathIeeeDoubBasBase`/
   `_MathIeeeSingTransBase`/etc - libnix's software float is implemented
   as calls into these standard AmigaOS libraries, not self-contained
   code. The bare AROS ROM this repo's `emu/` uses (a minimal 2015 AROS
   snapshot, just enough for `exec`+`dos`+boot) doesn't have them built
   in. This is a gap in that specific minimal boot ROM, not evidence
   that real classic AmigaOS (Kickstart 2.04+, which is what actual
   A500+/A4000 hardware ships) lacks them - it doesn't.
2. `devkit/extras/mathieee/` (a different workstream in this repo)
   had already run into the same gap and hand-written replacement
   `mathieeedoubbas.library`/`mathieeedoubtrans.library` in m68k asm.
   Combined with real `locale.library`/`mathieeesingtrans.library`
   binaries pulled from the full AROS distribution already vendored at
   `python/src/aros-m68k-download/extracted/Libs/` (not the minimal boot
   ROM - an actual disk install), all four libraries were copied into a
   `LIBS:` drawer on the test volume. This got past the "library failed
   to load" messages entirely (confirmed: they no longer appear), but
   `lua` still produced no output before the run was killed, this time
   with an empty output file rather than the four warning lines.
3. Rebuilding with `-m68881` (native 68881 FPU instructions, since the
   test rig's A4000/040 model has an FPU) still hung the same way.

An empty (rather than partial) output file is consistent with normal
C stdio block-buffering when redirected to a file - if the process was
killed before exiting normally, buffered `print()` output would never
get flushed, so "no output" does not distinguish "immediately stuck"
from "ran the whole test script and was killed one instruction before
the buffer flush." Diagnosing which of those it is - and whether it's a
libnix/AROS library-linkage mismatch, an FS-UAE FPU emulation gap, or
something else - is real follow-up work, not something resolved here.
Evidence from these runs (startup-sequence, FS-UAE logs, the
partial/empty output files) is kept in `emu-test/` for whoever picks
this up next.

**What this does and does not mean**: the binary is verified structurally
sound (`file` check, full link with no undefined symbols) and the Lua
logic + number-type patch are verified correct (host build). What is
*not* verified in this sandbox is the binary actually running to
completion on m68k AmigaOS. Real hardware or a fuller AROS/Workbench
install (rather than this repo's minimal test ROM) is the natural next
step to close that gap.

## Stdlib coverage

Registered via `linit.c` (unmodified): `base`, `package` (`require`),
`coroutine`, `table`, `io`, `os`, `string`, `math`, `utf8`, `debug`.

- **Verified working** (host build, `host/verify_output.txt`): base,
  string, table, math, coroutine.
- **Compiles and links, not exercised**: `utf8`, `debug`, `package`
  (`require`/`loadlib`) - no reason to expect these behave differently
  from stock Lua since no source changes touch them, but they weren't
  specifically tested.
- **Compiles, expected to be limited on AmigaOS**: `os.execute()` maps
  to C's `system()`, which is not a shell-fork model AmigaOS has; expect
  it to fail cleanly (libnix's `system()` returns -1) rather than do
  anything, same for `io.popen()`. `os.tmpname()` uses `tmpnam()` (the
  host linker even warns about this - same warning applies on the
  cross build). Everything else in `os`/`io` (file open/read/write,
  `os.time`, `os.date`, `os.getenv`) should work since libnix provides
  real `fopen`/`time`/`getenv`, but wasn't tested against actual AmigaOS
  file paths.
- **Not built at all**: dynamic library loading (`package.loadlib`,
  `require`-ing a `.so`/native extension) is compiled but inert -
  `luaconf.h` doesn't define `LUA_USE_DLOPEN` or `LUA_DL_DLL` for this
  platform (correctly - AmigaOS shared objects aren't a `dlopen()`
  model), so `loadlib.c` falls into Lua's own "dynamic libraries not
  enabled" stub. Pure-Lua modules via `require` still work fine, only
  native C extension modules are affected.

## Layout

```
lua/
  README.md                    this file
  build.sh                     cross + host build script
  src-lua/lua-5.4.7/           pristine upstream Lua 5.4.7 (lua.org)
  patches/
    luaconf-amiga.h            full annotated luaconf.h used by both builds
    luaconf-amiga.diff         unified diff against stock luaconf.h
    lua_libm_compat_amiga.c    the log2f shim (see above)
  bin/lua, bin/luac            cross-compiled AmigaOS hunk binaries
  host/lua                     host sanity binary
  host/verify.lua              the correctness test script
  host/verify_output.txt       its captured output
  build/                       scratch dir build.sh regenerates each run
  emu-test/                    FS-UAE on-Amiga attempt + evidence (see above)
```

## The amiports package

`ports/tree/lua/Portfile` and `ports/tree/luac/Portfile` package this as
two amiports ports (interpreter and compiler separately, matching how
upstream Lua ships them as two binaries). Both vendor their own copy of
the patched sources under `src/` per the ports system's "no
cross-port `..` references, no dependencies on other agents'
directories" rule (see `ports/FORMAT.md`) - so yes, the ~30 Lua source
files are duplicated between `lua/`, `ports/tree/lua/src/`, and
`ports/tree/luac/src/`. That is deliberate self-containment, not an
oversight.

The `cflags: -m68020 -lm` line overrides the harness's default
`-m68000` (the last `-m` flag on the command line wins, verified) and
adds the math library. Both ports were built through the real harness,
not just written and left untested:

```
$ cd ports && python3 harness/amiport-build.py build lua
  cross-compiling: m68k-amigaos-gcc -noixemul -m68000 ... -m68020 -lm -o lua src/lapi.c ...
  packaged lua-5.4.7.apak: 1 file(s), 234043 bytes, sha1 2d07a077...
$ file tree/lua/lua
tree/lua/lua: AmigaOS loadseg()ble executable/binary
```

and the same for `luac`. Both are in `ports/repo/index.txt` and
installable via `amiport install lua` / `amiport install luac` through
the existing amiports client, subject to the same on-Amiga-execution
caveat above.
