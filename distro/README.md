# RustChain Amiga Edition v1.0 - distribution pipeline

A modified AmigaOS distribution with the RustChain miner, tools, and SDK
preinstalled. "Make Amiga great again." Built by `assemble.sh` from the
Phase 1/2 outputs in this repo (miner/, tools/, sdk/, emu/).

## The two tiers (legal split, hard rule)

| Tier | Base OS | Redistributable | Artifact |
|------|---------|-----------------|----------|
| PUBLIC | AROS m68k (open source, APL) | YES | `images/RustChainAmiga.hdf` + `pack/rustchain-tools.lha` |
| PERSONAL | Workbench 3.1 (Scott's Amiga Forever 9 license) | **NEVER** | `personal/RustChainAmiga-WB31-PERSONAL-DO-NOT-DISTRIBUTE.hdf` |

Nothing from `emu/roms/licensed/` may reach a public artifact. `assemble.sh`
enforces this with a three-way audit (archive/image name listing, strings
scan, and byte-sample search using 64-byte entropy-checked samples from every
licensed ADF/HDF/ROM) and refuses the build on any hit.

## What gets built

- `pack/RustChain/` - plain drawer tree: `C/` (miner + rtc* tools), `SDK/`
  (librustchain headers + lib + examples), `S/` (user-startup snippet),
  `docs/`, and `Install_RustChain` (AmigaDOS installer for stock Workbench:
  copies the drawer to SYS: and appends the path line to S:user-startup,
  marker-guarded so it never double-installs).
- `pack/rustchain-tools.lha` - packed with `jlha` (Java LHA, apt
  `jlha-utils`; the classic `lha` package has no candidate on this Ubuntu and
  `lhasa` is extract-only). Round-trip verified against the tree on every
  build. `pack/rustchain-tools.zip` is built too as the fallback.
- `images/RustChainAmiga.hdf` - PUBLIC bootable image. 64 MB **plain FFS
  hardfile, no RDB** (DOS\1). Empirically verified: the AROS m68k ROM mounts
  and boots it directly via uaehf with no extra filesystem, and runs
  `S/startup-sequence` from it. Layout: `S/` (startup-sequence), `C/`
  (binaries), `SDK/`, `docs/`, `T/` (per-boot scratch log).
- `personal/make_personal_wb31.sh` - PERSONAL tier: copies the licensed
  workbench-311.hdf and injects the same RustChain drawer + S/user-startup
  line via xdftool. Output is gitignored and mode 600.

## Boot behavior of the public image

The startup-sequence prints the banner, then (guarded by
`IF EXISTS SYS:C/rustchain_amiga` so a missing binary cannot wedge boot)
runs a miner dry-run demo into `SYS:T/rustchain-lastrun.log` - one
overwritten log per boot, never an unbounded append. It then `ask`s
whether to start real mining, **default no** (plain Enter skips). Edit the
`--wallet` id in `S/startup-sequence` before answering y.

Only ROM-internal shell commands are used. Verified working in the AROS
boot shell: `echo`, redirection, `failat`, `path`, `if / else / endif`,
`exists`, `execute`, `ask`. NOT available: `version`. Gotcha: `*` is the
AmigaDOS escape character inside quoted strings - a `"***"` banner makes
echo fail with "argument line invalid or too long".

## How to boot

PUBLIC (works today, no owned ROMs needed):

    fs-uae configs/RustChainAmiga-public.fs-uae
    # headless:
    xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
        fs-uae configs/RustChainAmiga-public.fs-uae

PERSONAL (blocked on ROM decryption): `configs/RustChainAmiga-personal-wb31.fs-uae`
points at the personal HDF, but the Kickstart 3.1 ROMs in
`emu/roms/licensed/rom/` are Cloanto-encrypted and there is no `rom.key`
on this machine yet. Until the key exists the personal image is built and
verified by file inspection only, not boot-tested.

## Rebuild + test

    ./assemble.sh           # idempotent; re-run anytime. Missing inputs
                            # (tools/bin/*, sdk/) warn and are skipped,
                            # then picked up automatically on the next run.
    ./test/boot_test.sh     # copies the HDF, injects S/Test-Hooks (test-only,
                            # never shipped), boots headless under Xvfb,
                            # verifies the guest wrote proof to test/shared/
    ./personal/make_personal_wb31.sh   # personal tier (local only)

Evidence from the verification boots lives in `test/`: `shared/distro_boot.log`
and `shared/miner-dryrun.log` (written by the guest), extracted
`rustchain-lastrun.log`, and `screenshots/distro_boot_final.png` (AROS shell
showing the banner, the demo, and the mining prompt).

`MANIFEST.txt` (regenerated each assemble run) lists sizes + SHA-1s of all
public artifacts, the exact input binaries used, the pack tree, and any
missing-input warnings.

## Licensing

- AROS m68k ROMs: (c) AROS Development Team, AROS Public License (APL).
  Redistributable; SHA-1s + provenance in `../emu/roms/HASHES.txt`.
- RustChain miner, tools, SDK: RustChain repository license
  (github.com/Scottcjn/Rustchain).
- Amiga Forever / Workbench / Kickstart content: NOT included in any public
  artifact. Owners of AmigaOS install the tools pack with
  `Install_RustChain`; that is the supported path for real hardware.

## Honesty note

The public image runs under emulation and is expected to be flagged by the
RustChain server (ROM clustering + anti-emulation) and earn minimal rewards.
That is the system working as designed. Real vintage Amigas running the
tools pack get the real antiquity multipliers.
