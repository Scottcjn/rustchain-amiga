# RustChain for Amiga — Build Spec (e)
Date: 2026-07-01 · Regime: TRANSITIONAL (consensus-multiplier patch = draft only, CHAOTIC, not deployed)

## Goal
1. Port the RustChain miner to classic AmigaOS (m68k) as a small C program.
2. Test it inside an Amiga emulator (FS-UAE) with network emulation (bsdsocket + A2065/slirp NIC).
3. Document the "latest AmigaOS" upgrade path (3.2.3 classic / 4.1 FE PPC / AROS free).
4. Draft (NOT deploy) server-side m68k antiquity multipliers + verify ROM-clustering flags the emulator.

## Ground truth / constraints
- Node HTTP endpoint for legacy miners: `http://50.28.86.131:8088` (plain HTTP, proxies to 8099).
  Health: GET /health · Miners: GET /api/miners · Attest: POST /attest/submit
- Reference payload format: `~/tmp_rustchain/rustchain_mac_universal_miner_v2.2.2.py` and
  `~/tmp_rustchain/rustchain_linux_miner.py` (read these; match field names EXACTLY —
  remember the 2025-12-20 hardware_id field-name collision bug: server accepts both
  `model|arch|family` and `device_model|device_arch|device_family`).
- ROM fingerprint DB: `~/tmp_rustchain/rom_fingerprint_db.py` (Amiga Kickstart SHA-1s).
- NO Kickstart ROMs on this machine. Use AROS m68k ROM replacement (open source, legal).
  Do NOT download commercial Kickstart ROMs.
- Wallet/miner id for tests: `amiga-fsuae-scott` (emulated — EXPECTED to be flagged/minimal reward).

## Deliverables & layout (all under ~/rustchain-amiga/)
- `SPEC.md` (this file)
- `miner/rustchain_amiga_miner.c` — single-file C (C89/C99 compatible with bebbo gcc -noixemul or libnix)
  - Detects CPU via ExecBase->AttnFlags (68000/010/020/030/040/060, FPU bits)
  - Reads Kickstart version (ExecBase LibNode lib_Version) + SHA-1 of 512KB ROM at 0xF80000
    (implement tiny SHA-1 in-file; no external crypto lib)
  - Opens bsdsocket.library, HTTP/1.1 POST attestation JSON to node every N minutes
  - Flags: --test-only (print detection, no network), --dry-run (print payload),
    --node <host:port>, --wallet <id>  (honors the "don't trust, verify" doctrine)
  - Device payload: family="m68k", arch="68030" etc., model="Amiga (AROS/FS-UAE)" from detection,
    plus `rom_hash`, `rom_size`, `kick_version` fields for server ROM clustering
- `miner/Makefile` — cross-compile via bebbo amiga-gcc Docker image (try `bebbo/amiga-gcc`,
  fallbacks: walkero images / building vbcc+NDK). Output: `miner/rustchain_amiga` (Amiga hunk exe)
- `emu/fsuae/` — FS-UAE config: A1200 or A4000 model, AROS m68k ROM, `bsdsocket_library = 1`,
  A2065/slirp NIC option documented, directory hard-drive mapping `~/rustchain-amiga/shared/` as DH1:
  with startup-sequence auto-running the miner, output redirected to DH1:miner.log
- `docs/AMIGAOS_UPGRADE_PATH.md` — latest-OS research (3.2.3, 4.1 FE U3 + QEMU pegasos2, AROS)
- `server/m68k_multiplier_patch.py.draft` — draft additions to rip_200 multiplier table
  (proposed: 68000 3.5x, 68020 3.0x, 68030 2.8x, 68040 2.5x, 68060 2.2x — MYTHIC/LEGENDARY tier
  rationale in comments). DRAFT ONLY. Do not touch prod nodes.

## Acceptance (each must be demonstrated, not asserted)
A1. `rustchain_amiga` binary cross-compiles clean (hunk executable, `file` shows AmigaOS loadseg).
A2. FS-UAE boots AROS ROM headless-ish and runs the miner from startup-sequence.
A3. Miner --dry-run output visible in shared/miner.log with correct detection fields.
A4. Live attest: POST reaches 50.28.86.131:8088, response captured; miner appears in /api/miners
    OR is rejected/flagged with a server-side reason (either outcome = pass; document which).
A5. ROM hash reported matches AROS ROM, and we confirm what the clustering server would do with it.

## Hard rules
- Nothing deployed to prod nodes (50.28.86.131/.153, Ryan's, createkr's) without Scott's explicit OK.
- No pirated ROMs/OS images. AROS only, or document-what-to-buy.
- Re-read this spec each loop. Verify before declaring victory.

## PHASE 2 — RustChain Amiga Edition distro + SDK (added 2026-07-02, Scott's directive)
Goal: a modified AmigaOS distribution with preconfigured RustChain tools + an updated SDK. "Make Amiga great again."

Legal split (hard rule):
- PUBLIC tier: AROS m68k based bootable HDF/ADF — fully redistributable, ships with everything preinstalled.
- PERSONAL tier: Scott's licensed Workbench 3.1 (workbench-311.hdf from his Amiga Forever 9 Plus) with the same
  tools pack installed — NEVER redistributed; public users get the tools .lha + an installer script instead.

Workstreams (one agent each, separate dirs, no file overlap):
- sdk/      librustchain: factor miner into a static lib (bsdsocket HTTP, JSON build/parse, SHA-1, hw detect,
            attest client) + headers + examples + docker-toolchain Makefile + quickstart docs. This IS the
            "updated SDK" for Amiga RustChain development.
- tools/    First ported tools built on the SDK pattern: rtcwallet (balance/epoch/receive), rtcfetch (HTTP
            fetch utility), rtctop (network/miner status). Amiga hunk binaries + host tests.
- distro/   Distribution assembly: amitools (xdftool/rdbtool) pipeline building
            (a) rustchain-tools.lha pack, (b) RustChainAmiga-AROS.hdf bootable public image,
            (c) personal WB3.1 variant script (uses licensed hdf, output marked PERSONAL),
            (d) FS-UAE configs + README. assemble.sh must be re-runnable to pick up late tool binaries.
Acceptance: public HDF boots in FS-UAE and runs miner+tools; tools pack installs on stock WB3.1; nothing
licensed in any public artifact; everything re-buildable from source.

## PHASE 3 — Modern dev environment ON the Amiga (added 2026-07-02, Scott's directive)
Augment the AmigaOS distro with modern compilers plus Java and Python. Separate dirs, no overlap
with running agents (tools/, distro/ still building).
- devkit/   Native on-Amiga C toolchain: current vbcc (m68k-amigaos hosted) + NDK includes installed
            as a Development drawer for the distro; acceptance = compile and run hello.c INSIDE FS-UAE.
- python/   MicroPython port to m68k AmigaOS (check for existing ports first; cross-build with the
            docker toolchain); acceptance = REPL or script run inside FS-UAE printing proof.
- java/     Java via GCJ AOT (gcc 6.5 still contains it) targeting m68k-amigaos, fallback JamVM/Kaffe
            survey; acceptance = HelloWorld.java compiled and RUN inside FS-UAE, or an honest
            feasibility verdict with evidence.
Same hard rules: legal sources only, nothing to prod nodes, verify inside the emulator not just host.

## PHASE 4 — amiports: a MacPorts-style ports system for AmigaOS (added 2026-07-02, Scott's directive)
"We can create amiga ports like macports." Working name amiports (AmigaPorts org exists on GitHub; final
name is Scott's call). Dir: ports/. Design pillars:
- Portfile recipes (name, version, source url, sha1, build=cross-docker|native-vbcc, install layout, license).
- Host-side harness: builds recipes with the docker cross toolchain into .lha (or .zip) packages + an index.
- On-Amiga client `amiport` (C, links librustchain pieces): list/info/install/remove against an HTTP repo
  (rtcfetch-style bsdsocket GET + rc_sha1 verify + lha extract + local package db in SYS:).
- Bootstrap ladder doctrine: native vbcc is rung 1; ports formalize backporting newer compilers rung by rung.
Acceptance: amiport install of at least one real package works INSIDE FS-UAE from an HTTP repo served by the
host; repo layout documented so it can later live on node 1 nginx or GitHub releases.
