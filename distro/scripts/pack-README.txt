RustChain Tools for AmigaOS
===========================

This drawer holds the RustChain miner and tools built for classic
AmigaOS / AROS m68k (68020+ recommended, works from 68000 where noted).

Contents
--------
  C/            executables (Amiga hunk format)
    rustchain_amiga   the RustChain miner / attestation client
    rtcwallet         wallet: balance, epoch, receive address (if present)
    rtcfetch          small HTTP fetch utility (if present)
    rtctop            network / miner status viewer (if present)
  SDK/          librustchain headers + static lib for your own tools
                (present when the SDK build shipped with this pack)
  S/            user-startup snippet used by the installer
  docs/         this file and licensing notes
  Install_RustChain   installer script for stock Workbench

Install on stock Workbench (2.0+)
---------------------------------
1. Extract the archive somewhere (lha x rustchain-tools.lha).
2. Open a Shell, cd to where the RustChain drawer landed.
3. execute RustChain/Install_RustChain
   It copies the drawer to SYS:RustChain and adds SYS:RustChain/C
   to your path via S:user-startup (once, marker-guarded).

Try it (no network, no writes)
------------------------------
  rustchain_amiga --test-only     print hardware detection only
  rustchain_amiga --dry-run       print the exact attestation JSON
  rustchain_amiga --once --node 50.28.86.131:8088 --wallet YOUR-WALLET

Don't trust, verify: --test-only and --dry-run never touch the network.
Network use needs a bsdsocket stack (Roadshow/AmiTCP on real hardware,
bsdsocket_library = 1 under FS-UAE/WinUAE).

Honesty note: emulated Amigas are detected server-side (ROM clustering,
anti-emulation) and earn minimal rewards. Real vintage hardware is the
point. Emulators are still welcome for development and testing.

Licensing
---------
RustChain tools and SDK: see the RustChain repository license.
This pack contains no Commodore-licensed OS material - no ROMs, no
OS disk files. It installs onto an AmigaOS you already own, or
onto the free AROS-based RustChain Amiga Edition image.

Project: https://github.com/Scottcjn/Rustchain
