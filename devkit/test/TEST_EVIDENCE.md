# Devkit in-guest compile+run evidence

Claim proven: **vbcc 0.9h p3, running natively as 68k code inside the
emulated Amiga (FS-UAE 3.1.66, AROS m68k ROM, A4000/040 profile), compiled
a C program, and the compiled program executed inside the same guest with
correct output.** No cross-compilation was involved anywhere in the loop.

Final verification run: 2026-07-02, config `devkit-test.fs-uae`, clean
boot volume (only sources, devsetup, libs, startup-sequence present),
command:

    timeout 240 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
        fs-uae /home/scott/rustchain-amiga/devkit/test/devkit-test.fs-uae

All files under `evidence/final_*` were written BY THE GUEST into the
mapped boot volume (except final_fsuae_stdout.log, the emulator stdout).

## What the guest did (S/startup-sequence)

1. `FailAt 21`, `Stack 65536` (Stack is ROM-internal, proven:
   final_stackval.txt says "Current stack size is 65536 bytes").
2. Ran `SYS:devsetup` (compiled with this very toolchain): created
   RAM:Env, RAM:T and assigned ENV:, T:, vbcc: via dos.library calls.
   Proof: final_setup.log. Needed because the AROS ROM shell has no
   Assign/Makedir, and vc touching an unassigned volume pops an
   insert-volume requester that hangs a headless boot (root cause of the
   early test failures, found via probes in runs 5 and 6).
3. Manual pipeline (final_compile.log brackets each step):
   `vbccm68k -O=1` C to asm, `vasmm68k_mot -Fhunk` asm to object,
   `vlink` object+startup.o+vc.lib to executable, then ran `SYS:hello`.
   Output: final_hello_out.txt (marker, fib(20)=6765, sum=500500,
   COMPUTATION-OK).
4. vc frontend, one command:
   `vc +SYS:vc-sys.cfg -v -notmpfile -O1 SYS:hello.c -o SYS:hello2`.
   final_vc.log shows vc's own -v trace of the three child commands and
   "Size of executable: 8024 bytes"; final_hello2_out.txt shows the run.
5. Wrote "ALL DONE" (final_done.txt).

Both produced binaries are archived (final_hello_binary,
final_hello2_binary) and are `AmigaOS loadseg()ble executable/binary`.

## Fixes that were required (documented for the distro agent)

- **mathieee libraries**: vasm opens mathieeesingbas + mathieeedoubbas +
  mathieeedoubtrans at startup (strings in the binary + vamos trace). The
  AROS ROM only carries singbas (verified by strings on both ROM images),
  so vasm exited silently, no output at all (its error goes to stderr,
  which a redirected startup-sequence command loses). Fixed with the
  FPU-based replacement libraries in `devkit/aros-libs/`, placed in the
  boot volume `libs/` drawer.
- **Case-shadowing gotcha**: AROS auto-creates lowercase `c devs libs s`
  dirs on the boot volume at first boot. The host FS is case-sensitive,
  so creating `Libs/` from the host produced TWO dirs and the guest's
  LIBS: lookup can hit the empty one. Put files in the guest-created
  lowercase `libs/`.
- **vc hang**: vc's startup and config handling touch ENV:, T: and (in
  the no-arg case) vbcc:. Unassigned volume = requester = eternal hang
  when headless. devsetup fixes all three assigns. Probe evidence: run 6
  hung on bare `vc` with no arguments; run 7 (same command, after
  devsetup) exits with "No objects to link" as expected.
- **No Delete in ROM shell**: vc's cleanup lines report
  "delete: object not found", intermediates stay. Harmless, cosmetic.

## Earlier run archive

- run4_*: first successful manual pipeline (before vc worked). The
  run4/final hello binaries are bit-identical (same SHA-1,
  aeec30d48bd5a07cbe938cfb0b89a961a14856f4): the toolchain is
  deterministic across boots.
- run7_*: first fully green run including vc.

## Independence from the other workstreams

The rig boots its own volume `devkit/test/shared/` and mounts
`devkit/Development/` as DH1:. `emu/fsuae/rustchain.fs-uae` and the
top-level `shared/` were not modified.
