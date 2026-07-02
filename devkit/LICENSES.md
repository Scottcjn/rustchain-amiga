# Devkit licensing and provenance

Date: 2026-07-02. Everything below was downloaded fresh from the official
sources; SHA-1s are of the exact files in `devkit/downloads/`.

## vbcc 0.9h patch 3 (compiler, hosted Amiga binaries)

- Source: Volker Barthelmann's official vbcc site, sun.hasenbraten.de/vbcc
  (linked from www.compilers.de/vbcc.html), files served from
  phoenix.owl.de. Current release = 0.9h patch 3 (22-May-2022): the target
  archives were replaced on 2022-05-22, the hosted binary archives kept from
  patch 2 (2022-03-23). Frontend version string: `$VER: vbcc 0.908 (20.03.2022)`.
- Files:
  - `vbcc_bin_amigaos68k.lha` (2,512,493 bytes)
    SHA-1 `2e245649b55217a224d8b135877850dd3150425d`
    from http://phoenix.owl.de/vbcc/2022-03-23/vbcc_bin_amigaos68k.lha
  - `vbcc_target_m68k-amigaos.lha` (705,685 bytes)
    SHA-1 `c3ed907c9d9535508ea95c597fb0ecd6987412d6`
    from http://phoenix.owl.de/vbcc/2022-05-22/vbcc_target_m68k-amigaos.lha

License text (vbcc.pdf section 1.2, verbatim):

> vbcc is copyright in 1995-2022 by Volker Barthelmann.
> This archive may be redistributed without modifications and used for
> non-commercial purposes. An exception for commercial usage is granted,
> provided that the target CPU is M68k and the target OS is AmigaOS.
> Resulting binaries may be distributed commercially without further
> licensing. In all other cases you need my written consent.

vasm (vasm.pdf 1.2): same wording, copyright 2002-2022 Volker Barthelmann.
vlink (vlink.pdf 1.2): copyright 1995-2022 Frank Wille, same wording with
the commercial exception for AmigaOS/68k.

What that means for the distro:

- Redistributing the UNMODIFIED .lha archives is explicitly allowed.
- Programs users compile with it are unrestricted, even commercially
  (M68k/AmigaOS target).
- An EXTRACTED tree baked into a public HDF is a modified form of the
  archive and is NOT clearly covered.

**Tier ruling: SEMI-PUBLIC.** Two safe ways to ship it:
1. Preferred: include the two pristine .lha files on the public image plus
   an install script that extracts them on first boot (or at image-assembly
   time on the user's machine). The archives themselves stay unmodified.
2. Or ship nothing and let the user run `devkit/fetch_vbcc.sh`.
The ready-extracted `devkit/Development/` tree is fine for Scott's PERSONAL
images and for local use. If Scott wants the extracted drawer on the public
HDF, ask Volker Barthelmann (vb@compilers.de) for written consent first;
he has historically been friendly to Amiga distros that asked.

## NDK 3.2 Release 4 (AmigaOS includes and link libs)

- Source: Aminet `dev/misc/NDK3.2.lha`, uploaded by Hyperion Entertainment
  CVBA themselves (support@hyperion-entertainment.com), Revision 4,
  08.03.2022. Freely downloadable.
- File: `NDK3.2.lha` (8,448,365 bytes)
  SHA-1 `94c730f79c89febd2f337ece60f656c8499c104e`
- The archive's ReadMe-NDK.txt contains NO license or redistribution
  grant at all. Copyright Hyperion Entertainment.

**Tier ruling: PERSONAL.** Do not bake `NDK3.2/` into the public HDF.
Public users fetch it from Aminet with `fetch_vbcc.sh` (one curl). Only the
subset Include_H, Include_I, FD, SFD, lib was installed into the drawer;
the full archive stays in `devkit/downloads/`.

## mathieee replacement libraries (aros-libs/)

- `mathieeedoubbas.library`, `mathieeedoubtrans.library`
- Written from scratch for this devkit by Elyan Labs, 2026. m68k assembly,
  FPU-backed, sources in `devkit/extras/mathieee/`. NOT Commodore code,
  NOT AROS code, no third-party material.
- License: MIT. **Tier ruling: PUBLIC.** Ship anywhere.

## Example code and scripts

hello.c, devsetup.c, startup-sequence, configs, build scripts: Elyan Labs
2026, MIT. PUBLIC.
