# Java on classic m68k AmigaOS (RustChain Amiga Edition, Phase 3)

Status: WORKING, verified 2026-07-02 inside FS-UAE (AROS ROM, emulated 68040).

Real javac-produced Java 8 bytecode runs on classic AmigaOS through `mjvm`,
a micro-JVM class-file interpreter written for this project. GCJ ahead-of-time
compilation turned out to be infeasible for m68k-amigaos; the full ladder of
what was tried and why is in [FEASIBILITY.md](FEASIBILITY.md).

Be clear about what this is: mjvm executes a documented SUBSET of the JVM.
It is "Java on Amiga" in the sense that unmodified javac output for programs
inside that subset runs, with Java semantics, on the Amiga. It is not a full
Java platform, there is no class library beyond `System.out` printing.

## What works

- Compile on the host: `javac --release 8 HelloWorld.java` (any JDK 8..21+)
- Run on the Amiga: `mjvm HelloWorld.class`
- Verified inside FS-UAE booting the AROS m68k ROM on an emulated A4000/040:
  recursion (fib), loops, int arrays, tableswitch/lookupswitch, static method
  calls, System.out print/println. Output byte-identical to the host OpenJDK
  running the same class file. Evidence: [test/EVIDENCE.txt](test/EVIDENCE.txt).

## Layout

    java/
      Makefile              all build + test targets (see below)
      vm/mjvm.c             the whole VM, one ANSI C file, no OS deps
                            beyond stdio + malloc (builds with bebbo gcc
                            -noixemul/libnix and any host cc)
      vm/mjvm               AmigaOS hunk executable (build output)
      vm/mjvm_host          native host build (build output)
      hello/HelloWorld.java acceptance test program
      hello/HelloWorld.class  Java 8 bytecode (build output)
      test/java-test.fs-uae FS-UAE config (A4000/040 + AROS ROM + shared dir)
      test/shared/          mapped into the guest as the boot volume SYS:
        S/startup-sequence  auto-runs mjvm at boot, output -> SYS:java.log
        mjvm, HelloWorld.class  staged by `make stage`
        java.log            written BY THE GUEST during the test
      test/EVIDENCE.txt     archived proof of the verified run
      FEASIBILITY.md        the full rung-by-rung feasibility investigation

## Exact commands

    cd ~/rustchain-amiga/java

    make hello      # javac --release 8  -> hello/HelloWorld.class
    make host       # build mjvm natively, run the class on the host
    make amiga      # docker amigadev/crosstools:m68k-amigaos cross build
                    #   m68k-amigaos-gcc -noixemul -m68020 -O2 ...
                    #   -> vm/mjvm  (AmigaOS loadseg()ble executable)
    make stage      # copy mjvm + HelloWorld.class into test/shared/
    make test-emu   # boot FS-UAE headless (xvfb), guest runs the class,
                    # prints test/shared/java.log for host verification

Expected `make test-emu` guest output (test/shared/java.log):

    mjvm 0.1 micro-JVM (RustChain Amiga Edition, java/ Phase 3)
    mjvm: loaded HelloWorld (format 52.x, 5 methods, 58 cp entries)
    mjvm: --- program output ---
    RUSTCHAIN-JAVA-MARKER: mjvm running Java bytecode on AmigaOS
    fib(20)=6765
    sumOfSquares(10)=285
    pick(7)=70
    pick(3)=-1
    JAVA-TEST-COMPLETE
    mjvm: --- exit OK ---

## Runtime layout on a real Amiga / in the distro

mjvm is a single ~26 KB hunk executable with no dependencies beyond a stock
AmigaOS 2.0+ (or AROS) dos.library and the C runtime linked in statically
(libnix, -noixemul). Drop `mjvm` anywhere in the path (for the distro:
`SYS:Development/bin/mjvm`), put `.class` files next to your work, run:

    mjvm MyProgram.class

Compilation still happens on the host PC (javac targeting `--release 8`).
There is no on-Amiga compiler; see FEASIBILITY.md rung 1 for why not.
Exit code 0 on success, 20 (Amiga FAIL) on any VM error, with a one-line
diagnostic on stdout.

## The supported subset (honest list)

In:
- Single class file, format major <= 52 (Java 8). Newer class files are
  rejected with a clear message telling you to recompile with --release 8.
- `static` methods of that one class, `int` everywhere (boolean/byte/char/
  short are ints, as in real JVM stack semantics), `int[]` arrays,
  String literals.
- All int arithmetic/logic/shift/comparison bytecodes with Java semantics:
  32-bit wraparound, shift counts masked to 5 bits, idiv/irem truncate
  toward zero, INT_MIN/-1 = INT_MIN. tableswitch, lookupswitch, wide, iinc,
  goto/goto_w, all dup/pop/swap forms.
- `System.out` / `System.err` with `print`/`println` overloads for
  int, char, boolean, String, and the no-arg println.
- Recursion (VM frame depth limit 512).

Out (fatal error with a message, never silent wrong answers):
- long, float, double (bytecodes and descriptors rejected)
- objects, `new`, fields, other classes, interfaces
- string concatenation (javac compiles it to StringBuilder objects;
  print pieces separately instead)
- exceptions (try/catch), threads, monitors, invokedynamic/lambdas
- `args.length` (main receives null; the args array does not exist)

## m68k gotchas encoded in the design

- No 64-bit arithmetic anywhere in the VM. bebbo gcc 6.5 is known to
  miscompile 64-bit shifts at -m68000 (see miner/README.md for the war
  story); mjvm avoids the entire bug class instead of dancing around it.
- Built with -m68020 per project guidance. A -m68000 build should also be
  safe (no long long in the code) but has not been tested on a 68000.
- ILP32 everywhere: JVM slots are `unsigned int` (32-bit on both m68k and
  x86_64 hosts). References are tagged 32-bit handles, not pointers, so the
  same code runs on 64-bit hosts unchanged.
- Class files are big-endian. All multi-byte reads are explicit byte-compose
  shifts, so m68k (big-endian) and x86 hosts execute identical logic. The
  identical host/guest output is the regression test for this.
- No %lld, no %ld needed: everything printed is a 32-bit int.
