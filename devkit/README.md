# devkit -- modern native C toolchain ON the Amiga (Phase 3)

Mission: put a current C compiler on the Amiga itself, packaged as a
Development drawer for the RustChain Amiga Edition distro, and prove it
compiles and runs a program entirely inside the emulated Amiga.

**Status: PROVEN.** vbcc 0.9h patch 3 (current release, 2022-05-22),
running natively as 68k code inside FS-UAE on the AROS m68k ROM, compiled
`hello.c` and the compiled program ran and printed correct results, all
in-guest. Both the one-command `vc` frontend and the manual
vbccm68k/vasm/vlink pipeline work. Evidence in `test/evidence/`,
walkthrough in `test/TEST_EVIDENCE.md`.

## Layout

    devkit/
      Development/          the drawer as it should appear on the Amiga
        vbcc/               vbcc 0.9h p3: bin, config, targets, doc
        NDK3.2/             NDK 3.2 R4 subset: Include_H/I, FD, SFD, lib
        C/devsetup          assign helper for bare AROS ROM boots
        S/vbcc-startup      shell setup snippet for full OS installs
        examples/           hello.c, devsetup.c
        README              Amiga-facing plain-text readme
      aros-libs/            mathieeedoubbas/doubtrans.library (MIT, ours)
      extras/mathieee/      sources + build script for those libraries
      downloads/            pristine .lha archives + extraction staging
      fetch_vbcc.sh         re-download + rebuild the drawer from sources
      test/                 FS-UAE test rig + evidence (see below)
      LICENSES.md           provenance, license texts, tier rulings
      INSTALL_NOTES.md      real-hardware install guide

## For the distro agent -- how to merge

1. **PUBLIC AROS image**: do NOT bake the extracted `Development/` tree or
   `NDK3.2/` into the public HDF (license detail: vbcc allows redistributing
   only UNMODIFIED archives; the NDK has no redistribution grant at all;
   full reasoning in LICENSES.md). Instead ship:
   - `devkit/downloads/vbcc_bin_amigaos68k.lha`,
     `vbcc_target_m68k-amigaos.lha` verbatim (allowed) plus `fetch_vbcc.sh`
     or an extraction step that runs on the USER's machine at assembly time,
   - `aros-libs/mathieeedoubbas.library` and `mathieeedoubtrans.library`
     into the image's `Libs/` (MIT, ours, ship freely). Without these two,
     vasm dies silently on an AROS boot: the AROS ROM only carries
     mathieeesingbas.library. They need an FPU (the A4000/040 FS-UAE
     model has one).
   - `Development/C/devsetup`, `Development/S/vbcc-startup`,
     `Development/examples/`, `Development/README` (all ours, MIT).
2. **PERSONAL WB3.1 image**: merge the whole `Development/` drawer as-is.
   Workbench already has the real mathieee libraries, so skip aros-libs.
   `S/vbcc-startup` works there (Workbench has Assign/Path).
3. Boot integration on a bare-ROM AROS image: the ROM shell has NO
   Assign/Makedir/Delete. Run `Development/C/devsetup SYS:Development/vbcc`
   from startup-sequence before using `vc` (it creates ENV:, T: and vbcc:
   via dos.library; without it vc hangs forever on an insert-volume
   requester, which on a headless boot looks like a dead machine).
4. `Stack 65536` before compiling (Stack IS a ROM-internal command, verified).

## The test rig (test/)

`test/devkit-test.fs-uae` is a copy of the working emu config with its own
boot volume `test/shared/` (own startup-sequence) and a second directory
hard drive DH1: pointing at `Development/`. The original `emu/fsuae/`
config and top-level `shared/` were not touched.

Run it:

    timeout 240 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
        fs-uae /home/scott/rustchain-amiga/devkit/test/devkit-test.fs-uae

Watch `test/shared/` from the host: `compile.log`, `vc.log`,
`hello_out.txt`, `hello2_out.txt`, `done.txt` are all written by the guest.

## Host-side debugging trick

The native Amiga binaries can be exercised on the host with vamos
(amitools). Version pin that works: `pip install amitools machine68k==0.3.0`
(machine68k 0.4.x breaks amitools 0.8.1) and pass `-C 68020` (the vbcc
binaries crash on vamos' default 68000). Used to bisect the vasm failure
and to cross-build devsetup and the mathieee libraries with the devkit's
own toolchain. See `extras/mathieee/build_host.sh`.

## Known limits

- Binaries need a 68020+; that matches the distro's A4000/040 profile.
- On bare ROM boots without a Delete command, vc leaves .asm/.o
  intermediates next to the source. Cosmetic.
- The mathieee replacements implement the full documented API, but the
  transcendental functions use 6888x instructions that a REAL 68040/060
  traps to FPSP; fine under FS-UAE, fine on 68881/68882 hardware, and on
  real 040/060 machines the OS 68040/68060.library provides the trap
  handlers anyway. vasm never calls them when assembling integer code.
