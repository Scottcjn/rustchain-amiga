# Java on classic m68k AmigaOS: feasibility ladder

Phase 3 java/ workstream, RustChain Amiga Edition. Investigated 2026-07-02.

Verdict up front: **Rung 2 landed.** Real javac bytecode runs inside the
emulated Amiga via a purpose-built micro-JVM interpreter (`vm/mjvm.c`),
verified in FS-UAE with the AROS ROM (evidence: `test/EVIDENCE.txt`).
Rung 1 (GCJ AOT) is **not feasible** today, with evidence below.
Rung 3 (survey) is included for completeness.

Everything below states what was actually checked versus what is inference.

## Rung 1: GCJ ahead-of-time compilation — NOT FEASIBLE

The idea: gcc 6.5 was the last GCC with the Java frontend (gcj), and the
m68k-amigaos toolchain (bebbo's) is exactly gcc 6.5. If gcj could be enabled,
Java source could be AOT-compiled to native m68k Amiga code.

Checked facts, each verified directly on 2026-07-02:

1. **The shipping cross-toolchain has no gcj.** Inspected the
   `amigadev/crosstools:m68k-amigaos` docker image (the maintained build of
   bebbo's toolchain, gcc 6.5.0b). The bin directory contains
   m68k-amigaos-gcc/g++/cpp and binutils only. No gcj, no jc1, no jvgenmain,
   no ecj. (Command: `docker run --rm amigadev/crosstools:m68k-amigaos sh -c
   'ls $(dirname $(which m68k-amigaos-gcc))'`.)

2. **The amiga-gcc build system never enables Java.** The build Makefile
   (jeffv03/amiga-gcc, the surviving copy of bebbo's build scripts) line 181:

       CONFIG_GCC=--prefix=$(PREFIX) --target=m68k-amigaos
         --enable-languages=c,c++,objc ...

   Java was never part of the port. Nobody has ever configured or fixed the
   java frontend for this target.

3. **The patched gcc source is not even cloneable today.** The build script
   clones `https://github.com/bebbo/gcc` branch `gcc-6-branch` (Makefile line
   205). That repo returns 404 as of 2026-07-02 (bebbo's GitHub account is
   still up but lists a single public repo; amiga-gcc and the gcc fork are
   gone or private). Any "build it from source with java enabled" plan first
   has to source the amigaos-patched gcc tree from a third-party mirror.

4. **libgcj (the gcj runtime) has no amigaos support.** Checked
   `libjava/configure.host` in the upstream gcc-6.5.0 release: the only m68k
   line is `m68k-*) sysdeps_dir=m68k libgcj_interpreter=yes`, which exists to
   serve m68k *Linux/NetBSD*. There is no amigaos case anywhere in the file.
   libgcj hard-requires boehm-gc, libffi, and POSIX threads; none of the
   three has an m68k-amigaos port, and the bebbo toolchain provides no
   pthreads at all. This is the classic libgcj killer and it is fully present
   here.

5. **The "gcj without libgcj" partial win is smaller than it looks.**
   Two structural problems, from how gcj works in gcc >= 4.3:
   - gcj compiles .java by invoking the Eclipse compiler (ecj.jar) to
     bytecode first, then jc1 compiles the bytecode. So the java *frontend*
     alone additionally needs ecj.jar wired into the build
     (--with-ecj-jar), one more thing the amigaos build has never carried.
   - Even for a static-methods-only class, jc1-generated code emits
     `_Jv_*` runtime calls and expects libgcj's class metadata layout,
     vtables for java.lang.Object/Class, `_Jv_InitClass`, allocation and
     exception personality routines. A stub runtime satisfying that link
     surface is a real research project (bare-metal gcj experiments exist
     for other targets and are measured in weeks), not a timebox item.

Timebox decision: with facts 1-4 verified, attempting the full source build
(reconstructing bebbo's gcc tree from mirrors, wiring ecj, then watching
libjava fail to configure) had an expected value of a captured error message
at multi-hour cost. Declared not feasible on the evidence above and moved on,
per the priority ladder's instruction not to burn hours on a doomed
configure.

What WOULD make rung 1 viable someday: an m68k-amigaos port of libffi +
boehm-gc + a pthreads layer (AROS has some of this on other archs), plus a
maintained amigaos gcc tree. Nobody is close.

## Rung 2: tiny JVM interpreter — LANDED (this is the shipped result)

Survey of existing candidates first, then the build.

### Candidates surveyed

| VM | License | Why not |
|----|---------|---------|
| JamVM 2.x | GPL-2 | Written for Unix/POSIX: pthreads mandatory, per-arch native-ABI assembly or libffi for JNI dispatch. Supported arch list (x86, ARM, PowerPC, MIPS, SPARC...) does not even include m68k-linux, let alone amigaos. Porting = new threads layer + new ABI glue. Sources: jamvm.sourceforge.net, github.com/jserv/jamvm. |
| Kaffe | GPL-2 | Historic m68k support was real but OS-specific: config/m68k existed for Linux/NetBSD/NeXT with jthreads built on Unix signals/sigaltstack and a Unix syscall abstraction. The AmigaOS gap is the whole OS layer (threads, IO, net). Project is long dead; resurrecting it for amigaos is a bigger port than writing a small VM. |
| miniJVM (digitalgust) | MIT | Closest modern candidate: designed for small platforms, MIT licensed. But it requires a C11-threads-style layer (tinycthread: pthreads or win32 underneath), 64-bit long math throughout, plus its bundled class library. AmigaOS has neither threads API; a port means writing an exec.library-based threads shim and auditing 64-bit ops against the bebbo -m68000 miscompile. Plausible future work, over this timebox. |
| uJ / NanoVM / HaikuVM (microcontroller JVMs) | restrictive or GPL | uJ is non-commercial-licensed; NanoVM/HaikuVM do not execute .class files directly (host-side converter, tiny fixed runtime) and are AVR-shaped. Wrong fit. |

Conclusion of the survey: nothing existing drops in. The mission brief's own
fallback ("a class-file interpreter that only needs stdio+malloc") is the
right shape, so that is what was built, in-repo, with no license entanglement.

### What was built: mjvm

`vm/mjvm.c`: a single-file ANSI C class-file interpreter, ~700 lines.
Full constant-pool parser (all tags through 20, correct two-slot handling of
long/double entries), Code-attribute extraction, and an interpreter for a
documented int+String+int[] subset with real Java semantics (32-bit wrap,
masked shifts, truncating idiv, INT_MIN/-1, tableswitch/lookupswitch, wide).
Fatal, descriptive errors for everything outside the subset; never a silent
wrong answer. Design details and the exact in/out list: `README.md`.

Key m68k decisions:
- Zero 64-bit arithmetic (designs out the bebbo -m68000 64-bit-shift
  miscompile documented in miner/README.md). No Java long/float/double.
- ILP32-safe: slots are 32-bit tagged handles, not pointers, so identical
  code runs on x86_64 hosts (host test) and m68k (target).
- Endian-neutral byte-compose readers for the big-endian class format.

### Evidence chain (each step captured)

1. `javac 21.0.11 --release 8` produced `HelloWorld.class` (format major 52).
2. Host build (`cc -std=c89 -Wall -Wextra`, zero warnings) runs the class:
   fib(20)=6765, sumOfSquares(10)=285, switch cases correct.
3. Host OpenJDK (`java HelloWorld`) output is byte-identical for the program
   lines. Same class file, two independent executions.
4. Cross build: `m68k-amigaos-gcc -noixemul -m68020 -O2 -Wall -Wextra`,
   zero warnings; `file` says `AmigaOS loadseg()ble executable/binary`
   (26584 bytes).
5. FS-UAE 3.1.66 booted the AROS m68k ROM on an emulated A4000/040 with
   `java/test/shared/` as the boot volume; S/startup-sequence ran
   `SYS:mjvm SYS:HelloWorld.class >SYS:java.log`. The guest wrote
   `java.log` (with FS-UAE .uaem sidecar proving guest origin) containing
   the identical output, plus `done_proof.txt` after mjvm exited, proving
   clean termination. Full transcript: `test/EVIDENCE.txt`.

The marker + computation requirement is satisfied: fib(20) is computed by
recursive bytecode execution in the guest, not echoed.

### Honest limits of the claim

This is Java *bytecode* execution on Amiga with Java semantics for a subset.
There is no class library, no objects, no GC (only method frames and int
arrays are ever allocated; arrays are never freed until exit), no JIT
(interpretation on a 68040 is fine for utility-scale programs; fib(20) is
instant in the emulator). Programs must be written inside the subset. The
subset is enforced with clear fatal diagnostics, so a user finds out
immediately, not subtly.

## Rung 3: historical survey (context, not needed for the verdict)

- **JAmiga** (2005-2015, Peter Werno / Joakim Nordstrom): partial JVM for
  AmigaOS 4 on PPC, GPL. Never complete (classlib gaps), never m68k. Dead
  since ~2015.
- **Kaffe on Amiga**: no evidence of a completed classic-AmigaOS port was
  found; the m68k support that existed targeted Unix variants (see rung 2
  table).
- **AmigaOS 4.1 FE on QEMU (pegasos2/sam460ex) route**: OS4 is PPC; there is
  still no OpenJDK port for AmigaOS 4 (AmigaOS 4 Java efforts stalled at
  JAmiga). So even the "modern Amiga" path has no real Java; the common
  answer in that community is running JDK programs on a hosted Linux, which
  is not Java-on-AmigaOS at all.
- On classic m68k the commercial era produced no shipping JVM either (the
  1990s "Kaffe for Amiga" and "P'Jami" rumors never became releases we can
  cite binaries for). As far as this investigation could determine, mjvm
  running verified javac bytecode inside AROS/FS-UAE is a genuinely rare
  event for the platform.

## Future work (ranked)

1. Grow mjvm: static fields, string concat via a tiny built-in StringBuilder
   shim, long support (needs a 64-bit softpath audited against the bebbo
   shift bug), multiple classes from a directory classpath. Each is
   incremental; none blocks the current acceptance.
2. miniJVM port (MIT): exec.library threads shim + tinycthread backend +
   64-bit audit. The realistic road to "full" Java 8 subset with a class
   library if anyone needs it.
3. GCJ resurrection: only worthwhile after someone ports libffi+boehm-gc to
   m68k-amigaos. Not recommended.
