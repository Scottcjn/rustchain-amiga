# RustChain Amiga emulator environment (FS-UAE + AROS)

Status: WORKING, verified 2026-07-01 on this box.

## What is installed

- FS-UAE 3.1.66 from Ubuntu apt (`fs-uae 3.1.66-2build2`, questing/universe).
- AROS m68k Kickstart replacement ROMs (open source, APL license), extracted
  from the fs-uae package's own data bundle `/usr/share/fs-uae/fs-uae.dat`.
  No commercial Kickstart ROMs anywhere on this machine.
  - `roms/aros-amiga-m68k-rom.bin` (512 KB main ROM)
  - `roms/aros-amiga-m68k-ext.bin` (512 KB extended ROM)
  - SHA-1s and provenance: `roms/HASHES.txt`
  - The booted AROS identifies itself as "Version SVN50730, built on 2015-05-20".

## How to run it

Integration run (window on the desktop):

    fs-uae /home/scott/rustchain-amiga/emu/fsuae/rustchain.fs-uae

Headless (proven working):

    xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
        fs-uae /home/scott/rustchain-amiga/emu/fsuae/rustchain.fs-uae

FS-UAE prints a short log on stdout and writes the detailed log to
`~/Documents/FS-UAE/Cache/Logs/fs-uae.log.txt` (overwritten each run).
Boot to the AROS shell prompt takes well under 60 seconds; startup-sequence
output files appear in `shared/` within ~30 seconds of launch.

Evidence from the verification runs is archived in `fsuae/BOOT_EVIDENCE.txt`
and `fsuae/screenshots/aros_boot_75s.png` (AROS shell visible).

## Machine configuration (fsuae/rustchain.fs-uae)

- Model A4000/040: 68040 CPU, 32-bit addressing (required for Zorro III RAM).
- 2 MB chip RAM + 32 MB Zorro III fast RAM.
- `bsdsocket_library = 1` (verified in log: "bsdsocket.library installed",
  "Creating UAE bsdsocket.library 4.1").
- `uae_a2065 = slirp`: A2065 Zorro II Ethernet with built-in SLIRP user-mode
  NAT. VERIFIED available and initializing in this build (log:
  `A2065: 'slirp' 00:00:00:32:33:34`, Zorro card mapped at 0xE90000).
  Unused by the miner (see Networking below) but left enabled since it is
  harmless.
- `hard_drive_0 = /home/scott/rustchain-amiga/shared` mapped as a directory
  hard drive, device DH0:, volume name "RustChain", read-write, bootable.

## How the miner auto-runs

AROS boots from ROM, mounts DH0: (the `shared/` directory), and because that
volume is the highest-priority bootable device, DOS runs
`S/startup-sequence` from it using the ROM-internal shell. This is exactly
the classic AmigaOS boot flow; no Workbench files are needed on the volume,
the AROS ROM carries the shell and internal commands (echo, redirection,
program launch).

`shared/S/startup-sequence` currently does:

1. `echo "RustChain Amiga environment boot OK" >SYS:boot_proof.txt`
   (host-visible proof the guest booted; verified working)
2. A PLACEHOLDER echo into `SYS:miner.log`, because the miner binary is not
   built yet (that is the miner agent's deliverable). Once
   `shared/rustchain_amiga` exists, uncomment these lines in the script:

       SYS:rustchain_amiga --dry-run >SYS:miner.log
       SYS:rustchain_amiga --once --node 50.28.86.131:8088 --wallet amiga-fsuae-scott >>SYS:miner.log

   and delete the placeholder echo line.

Gotchas learned the hard way:

- Use `SYS:` (the boot volume) in the script, not `DH0:` or `DH1:`. SYS:
  always resolves to whatever volume booted, so the script keeps working if
  the drive index or label changes. (SPEC.md mentions DH1:; with this config
  the directory actually mounts as device DH0:, volume "RustChain". SYS:
  sidesteps the mismatch entirely.)
- `version` is NOT a ROM-internal command in the AROS boot shell; it fails
  with "object not found". Stick to echo, redirection, and launching the
  miner binary.
- FS-UAE writes a `.uaem` metadata sidecar file next to every file the guest
  creates in a directory hard drive. Ignore them (do not parse them as logs).
- Case: AROS/AmigaOS filesystems are case-insensitive; `S/startup-sequence`
  works as `S:Startup-Sequence`.

## Networking: how bsdsocket maps to the host

`bsdsocket_library = 1` implements the AmigaOS `bsdsocket.library` API inside
the emulator. Guest socket calls (socket/connect/send/recv, DNS) are executed
directly as host Linux socket calls by the FS-UAE process. Consequences:

- The guest shares the host's network identity. No NAT, no guest IP, no
  TCP stack needed inside AROS. If the host can reach an address, the
  guest can.
- The miner just opens `bsdsocket.library`, connects to 50.28.86.131:8088,
  and it works exactly like a host curl.
- Host sanity check (verified 2026-07-01):

      curl http://50.28.86.131:8088/health
      -> {"ok":true,"version":"2.2.1-rip200",...}

The A2065/slirp path is the alternative "real NIC emulation" route: SLIRP
gives the guest a private NATed subnet (10.0.2.x, gateway 10.0.2.2, DNS
10.0.2.3, same scheme as QEMU user networking). To actually use it the guest
would need a SANA-II a2065 driver plus a TCP stack (AmiTCP, Roadshow, or
AROS's network stack) installed on the boot volume. The bare AROS ROM boot
does not include one, so the miner uses bsdsocket. Keep the A2065 line for
future full-OS installs; host-to-guest port forwards would use
`uae_slirp_redir = tcp:HOSTPORT:GUESTPORT`.

## Layout

    emu/
      README.md                this file
      roms/
        aros-amiga-m68k-rom.bin
        aros-amiga-m68k-ext.bin
        HASHES.txt              SHA-1s + provenance + clustering notes
      fsuae/
        rustchain.fs-uae        FS-UAE config (commented)
        BOOT_EVIDENCE.txt       captured logs from the verification boots
        fsuae_boot_test.log     stdout of the first headless boot
        slirp_test.log          stdout of the a2065=slirp verification run
        screenshots/
          aros_boot_75s.png     AROS shell at T+75s (headless Xvfb run)
    shared/                     mapped into the guest as DH0:/SYS:
      S/startup-sequence        auto-run script (placeholder miner line)
      boot_proof.txt            written BY THE GUEST at boot (proof)
      miner.log                 miner output lands here

## ROM clustering note for the server-side test

The AROS ROM SHA-1s (see roms/HASHES.txt) are not in
`~/tmp_rustchain/rom_fingerprint_db.py` today. Every FS-UAE/WinUAE user of
this AROS build reports the identical hash, so once 3+ miners report it the
clustering server flags them, which is the intended anti-emulation outcome
for this experiment. The emulated miner `amiga-fsuae-scott` is EXPECTED to be
flagged or earn minimal rewards.
