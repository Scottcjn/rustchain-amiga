# AmigaOS Upgrade Path (mid-2026)

Research doc for the RustChain-on-Amiga build (see `../SPEC.md`). Covers what the
current "latest AmigaOS" actually is on each line, what it costs, and the legal way
to run each one in emulation. All prices checked June/July 2026 and may drift.

Hard rule from SPEC.md: no pirated ROMs or OS images. Everything below is either
free/open source (AROS) or a documented legal purchase.

---

## 1. Classic 68k line: AmigaOS 3.2.x (Hyperion Entertainment)

**Latest point release: AmigaOS 3.2.3, released April 2025** (this is "Update 3"
for 3.2). Release history of the 3.2 line: 3.2 (May 2021), 3.2.1 (Dec 2021),
3.2.2 (Mar 2023), 3.2.3 (Apr 2025). Updates are free downloads for registered
3.2 owners via Hyperion's site / AmiUpdate. AmigaOS 3.3 is in development with
Hyperion targeting a 2026 release.

3.2.3 targets all classic Amigas with 680x0 CPUs, explicitly including machines
accelerated with PiStorm boards. (Attestation note: PiStorm is an ARM board
emulating a 68k in software (Emu68/Musashi). For RustChain purposes a PiStorm
machine is NOT real 68k silicon and should fingerprint as emulated/hybrid.)

**What 3.2 adds over the 1994-era 3.1** (high level): updated Workbench and
Kickstart with hundreds of fixes, ReAction GUI components, a help system, native
ADF disk-image mounting, CDFS CD-ROM support out of the box, updated Shell and
datatypes, and continued maintenance releases. It is the actively maintained OS
for real 68k hardware.

**Price and where to buy:**
- Digital download: **EUR 44.95 incl. VAT**, sold via Hyperion's partner 2Checkout,
  available since 23 March 2026 (sales had been paused during the
  Hyperion vs Amiga Corporation legal fight, which both parties stepped back
  from in March 2026).
- Physical CD box set still sold by dealers (amiga-shop.net, Alinea/ACube,
  AmigaKit, Amiga on the Lake, 8-Bit Classics).
- Important for emulation: **the AmigaOS 3.2 CD includes the Kickstart 3.2 ROM
  sets for every Amiga model ever produced.** Buying AmigaOS 3.2 once gives you
  legal Kickstart 3.2 ROM images you can point FS-UAE or WinUAE at
  (`kickstart_file = <path to A1200/A4000 3.2 ROM>`). No separate ROM purchase
  needed for emulation. Physical ROM chips for real machines are a separate
  product (e.g. "AmigaOS 3.2 ROM" at amiga-shop.net) for machines without
  soft-kick capability.

**Emulation path:** FS-UAE or WinUAE, A1200 or A4000 model, purchased Kickstart
3.2 ROM file, install from the purchased 3.2 CD ISO. Fully legal, roughly
EUR 45 all-in if you already have the emulator.

Sources:
- https://www.hyperion-entertainment.com/index.php/news/1-latest-news
- https://www.theregister.com/2025/04/10/amigaos_3_2_3/
- https://www.generationamiga.com/2026/03/25/amigaos-3-2-returns-as-hyperion-and-amiga-corp-step-back-from-lawsuit/
- https://theoasisbbs.com/amigaos-3-3-development-nears-completion/
- https://www.amiga-shop.net/en/Amiga-Software/AmigaOS-Amiga-operation-systems/AmigaOS-3-2-Rom::1069.html

---

## 2. Next-gen PPC line: AmigaOS 4.1 Final Edition (Hyperion)

**Latest version: AmigaOS 4.1 Final Edition with Update 3** (Update 3 is the
current cumulative update, free for registered owners as `AmigaOS4.1Update3.lha`
via Hyperion). 4.1 FE runs only on PowerPC: classic Amigas with PPC accelerator
cards, Sam440/Sam460, Pegasos 2, AmigaOne models, and X1000.

**Price:** physical copies are sold out; **digital download EUR 39.95 incl. VAT**
via 2Checkout, for both the "Pegasos 2" and "Classic" editions (you register the
serial at hyperion-entertainment.com and download).

**The realistic emulation route on x86 Linux is QEMU:**
- `qemu-system-ppc -M pegasos2` (QEMU >= 6.1; 8.x or newer strongly recommended)
  or `-M sam460ex`.
- **sam460ex**: the U-Boot-based firmware ROM ships WITH QEMU, so nothing extra
  to obtain; buy the Sam460 edition of 4.1 FE and install.
- **pegasos2**: the original Pegasos II SmartFirmware ROM is copyrighted and not
  redistributable; it can be legally extracted from Genesi's `up050404` firmware
  update with a documented script, OR skipped entirely by using the **BBoot**
  bootloader written by QEMU's PPC maintainer (Zoltan Balaton), which boots the
  4.1 FE Pegasos 2 ISO directly with no firmware ROM and no ISO modification.
  This is the easiest path since QEMU 8.1.
- Graphics: use the siliconmotion502.chip driver (in the Sam460 edition or in
  Update 3) for accelerated display under QEMU.

So: newest AmigaOS running on the Linux boxes in the fleet = QEMU pegasos2 +
BBoot + purchased 4.1 FE Pegasos 2 digital download. About EUR 40 total.

(Attestation note: this is PPC emulation on x86. It would and should be flagged
by RIP-PoA anti-emulation, same as FS-UAE. It is a dev/testing target, not a
mining target.)

Sources:
- https://www.qemu.org/docs/master/system/ppc/amigang.html
- https://zero.eik.bme.hu/~balaton/qemu/amiga/ (and aos_pegasos2.html there)
- https://m.amigaos.net/index.php/news/36-amigaos-4x/322-amigaos-41-final-edition-for-pegasos-2-is-now-available-digitally
- https://www.hyperion-entertainment.com/index.php/where-to-buy/direct-downloads/174-amigaos-41-final-edition-for-classic
- https://www.hyperion-entertainment.com/index.php/news (Update 3 announcement)

---

## 3. Free path: AROS (what this project uses for the emulator test)

AROS is the open source (APL/BSD-style licensed) reimplementation of the
AmigaOS 3.1 API. Cost: **$0**, no ROMs to buy, no legal risk. Three pieces
matter to us:

- **AROS m68k Kickstart replacement**: `aros-amiga-m68k-rom.bin` +
  `aros-amiga-m68k-ext.bin`. A drop-in open source Kickstart substitute for
  UAE-family emulators (WinUAE and FS-UAE actually bundle a build of it as the
  built-in "AROS ROM"). First workable release was 2011 (Kickstart replacement
  bounty); still maintained in AROS nightlies. This is exactly what SPEC.md
  mandates for the FS-UAE miner test. Caveat: high API compatibility but not
  100% of commercial Kickstart behavior; fine for our C miner using exec.library
  + bsdsocket.library.
- **AROS One**: the currently active, maintained AROS distribution.
  **AROS One v2.5** is the latest x86 release (2026); there is also an
  **AROS One 68k** variant usable on emulated/real Amigas.
- **Icaros Desktop**: the historically popular x86 AROS distro. Last release was
  2.3 (Dec 2020); the distribution went unavailable in November 2025 and is
  effectively discontinued. Do not build anything new on Icaros; use AROS One
  or plain AROS nightlies.

Sources:
- https://aros.sourceforge.io/download.html
- https://en.wikipedia.org/wiki/AROS_Research_Operating_System
- https://www.arosworld.org/ (AROS One releases)
- https://archiveos.org/icaros-desktop/
- https://eab.abime.net/showthread.php?t=56211 (AROS m68k Kickstart substitute)

---

## 4. Cheap legal ROM source: Amiga Forever (Cloanto)

Cloanto's Amiga Forever bundles licensed Kickstart ROMs (1.x through 3.x) plus
Workbench/OS files and preconfigured emulation. Current major version is
**Amiga Forever 11** (11.2.x point releases current in 2026). Pricing:

| Edition | Price | Notes |
|---------|-------|-------|
| Value | $19.95 | Downloadable installer, basic preconfigured emulation |
| Plus | $39.95 ($29.95 upgrade) | The one to get: full ROM/OS file set usable in external emulators (FS-UAE/WinUAE), plus DVD ISO |
| Premium | $59.95 ($40 upgrade) | Plus edition + boxed media + video DVDs |

Practical note: for pointing FS-UAE at legal Kickstart 1.3/3.1 ROM files, the
**Plus edition at $39.95** is the standard answer. Two caveats for RustChain:
(1) Amiga Forever ships Kickstart up to 3.x classic ROMs; Kickstart **3.2**
comes from Hyperion's AmigaOS 3.2 purchase, not from Cloanto. (2) Cloanto's
slightly modified 3.1 ROM is already in our known-emulator hash DB
(`rom_fingerprint_db.py`, SHA-1 `c3c48116...`), so a miner attesting with an
Amiga Forever ROM is still correctly flagged as emulated.

Sources:
- https://www.amigaforever.com/value/
- https://www.amigaforever.com/plus/
- https://www.amigaforever.com/premium/

---

## 5. Recommendation table for Scott

| Goal | Path | Cost | Notes |
|------|------|------|-------|
| (a) Test miner on emulated classic Amiga | FS-UAE + AROS m68k replacement ROM (built in) | **$0** | This is the SPEC.md path. Legal, zero purchases. Emulation WILL be flagged by RIP-PoA; that is the point of acceptance test A4/A5. |
| (b) Authentic AmigaOS 3.2.3 experience in emulation | Buy AmigaOS 3.2 digital (EUR 44.95, includes Kickstart 3.2 ROMs for all models) + free 3.2.3 update, run in FS-UAE/WinUAE | ~EUR 45 (~$49) | One purchase covers the OS AND the emulator ROMs. Cheaper alternative if 3.1 is enough: Amiga Forever Plus $39.95. |
| (c) Newest AmigaOS (4.1 FE Update 3) on x86 Linux | QEMU `-M pegasos2` + BBoot + AmigaOS 4.1 FE Pegasos 2 digital (EUR 39.95) | ~EUR 40 (~$44) | sam460ex is the no-extra-firmware alternative. Dev target only; PPC-on-x86 emulation, never a mining reward target. |
| (d) REAL 68k hardware attestation | Buy actual hardware (none in the Elyan fleet today) | see below | Only real silicon earns the proposed m68k multipliers. |

### Real hardware hunt list (pawn shops / eBay / AmiBay)

The Elyan fleet currently has zero Amigas, so real-hardware m68k attestation
needs a purchase. What to hunt, with rough mid-2026 going rates (eBay/AmiBay;
volatile, condition and recap status move prices a lot):

- **Amiga 1200** (68EC020 stock, trapdoor accelerator slot): the best target.
  Working unmodified units roughly $350-600; recapped/tested toward the high
  end. Boxed with a 68030 accelerator already fitted appears on AmiBay
  periodically and is usually the best value per dollar.
- **68030 accelerator for A1200** (Blizzard 1230-III/IV, Apollo/Turbo 1230):
  the Blizzard 1230-IV is the reference '030 board. Sold listings around
  EUR 250-350 for a 50 MHz 030 + FPU + 32-64MB fast RAM board alone. New-made
  alternatives (TF1230 and similar) are cheaper and give the same real-68030
  silicon.
- **Amiga 600** (68000 7 MHz, compact, cheap): roughly $150-350 working. Fine
  for a real-68000 (proposed 3.5x tier) data point, but no easy accelerator
  path except PiStorm, and **PiStorm = ARM emulation = flagged**, defeating the
  purpose for attestation.
- Avoid for attestation purposes: PiStorm/PiStorm32-equipped machines (ARM
  Emu68 under the hood), Vampire/Apollo 68080 FPGA boards (FPGA core, not
  Motorola silicon; needs its own policy decision), and any "Amiga in a box"
  mini/emulation products (A500 Mini etc., all software emulation).

Bottom line: cheapest credible real-hardware play is a working A1200 plus a
real-68030 accelerator, realistically $500-800 total on the current market, or
a bare A600 around $200 if we just want one genuine 68000 attester on the
network.

Sources:
- https://www.amibay.com/threads/amiga-1200-boxed-with-68030-accelerator-excellent-condition.2449414/
- https://www.amibay.com/threads/phase-5-blizzard-1230-iii-32mb-edo-ram-accelerator-for-amiga-1200.2441027/
- https://www.worthpoint.com/worthopedia/amiga-blizzard-1230-mk-iv-68030-50mhz-247722242
- https://www.ebay.com/sch/i.html?_nkw=amiga+accelerator

---

*Prepared 2026-07-01 for the rustchain-amiga build. Prices/versions verified by
web search on that date; re-check before purchase.*
