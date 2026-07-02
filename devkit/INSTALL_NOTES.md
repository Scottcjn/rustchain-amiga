# Installing the devkit on a real Amiga

For people running real hardware (or a full Workbench under emulation)
rather than the RustChain test rig.

## What you need

- Amiga with a 68020 or better (A1200, A3000, A4000, accelerated A500...).
  The vbcc 0.9h binaries are compiled for 68020+; they will crash on a
  stock 68000 A500. RAM: 8 MB fast recommended, compiles are hungry.
- AmigaOS 2.0 or newer (3.1/3.2 ideal), or a full AROS m68k install.
- The two vbcc archives and the NDK (get them yourself, it is free):
  - http://phoenix.owl.de/vbcc/2022-03-23/vbcc_bin_amigaos68k.lha
  - http://phoenix.owl.de/vbcc/2022-05-22/vbcc_target_m68k-amigaos.lha
  - http://aminet.net/dev/misc/NDK3.2.lha
  On a PC, `devkit/fetch_vbcc.sh` downloads and checksums all three.

## Install, the official way

Both vbcc archives contain an Installer script; run `Install` from each
archive on the Amiga and answer the prompts (install dir, target
m68k-amigaos, includes/libs assigns). Then unpack NDK3.2 somewhere and add
its includes to the include assign:

    Assign vincludeos3: Work:NDK3.2/Include_H ADD

## Install, the drawer way (what the RustChain distro uses)

Copy the `Development/` drawer to SYS: (or anywhere; adjust paths) and add
to S:user-startup:

    Execute SYS:Development/S/vbcc-startup

Open a new shell and:

    Stack 65536
    vc +aos68k -O1 hello.c -o hello
    hello

## Notes for real hardware

- Stack: give the compiling shell 64 KB (`Stack 65536`). Compiler crashes
  and silent wrong output are the classic small-stack symptoms.
- Math libraries: Workbench 2.0+ ships mathieeedoubbas/doubtrans.library
  in LIBS:, so vasm just works. The FPU replacement libraries in
  `aros-libs/` are ONLY for bare AROS ROM environments; do not overwrite
  the originals on Workbench. If you do use them: they need an FPU, and on
  a 68040/060 the standard 68040.library/68060.library must be installed
  (SetPatch does this) so trapped FPU instructions get emulated.
- Floating point: the default config uses softfloat (portable). For FPU
  code use `vc +aos68k -fpu=68881 ...` and link m881.lib, see vbcc.pdf
  section 14.5.2.
- 68060 users: vbcc emits plain 020 code by default, safe everywhere.
  `-cpu=68060` for tuned output.
- Editors: the drawer intentionally ships no editor; use Ed, or grab
  something proper (Vim and MicroEMACS exist for 68k on Aminet).
