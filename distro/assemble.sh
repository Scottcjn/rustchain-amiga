#!/usr/bin/env bash
# RustChain Amiga Edition - distribution assembler (Phase 2)
# Idempotent: re-run anytime; picks up tools/sdk binaries as they appear and
# warns (does not fail) when an optional input is missing.
#
# Builds:
#   pack/rustchain-tools.lha (+ .zip + plain drawer tree pack/RustChain/)
#   images/RustChainAmiga.hdf   public bootable AROS-based image (plain FFS)
#   MANIFEST.txt                sizes + SHA-1s of everything shipped
#
# Hard rule: NOTHING from emu/roms/licensed/ may reach pack/ or images/.
# An audit stage at the end enforces it and fails the build if violated.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # ~/rustchain-amiga
D="$ROOT/distro"
LICENSED="$ROOT/emu/roms/licensed"
XDF="$D/venv/bin/xdftool"
WARNINGS=()

say()  { echo "[assemble] $*"; }
warn() { echo "[assemble] WARN: $*" >&2; WARNINGS+=("$*"); }
die()  { echo "[assemble] FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------- stage 0: tooling
if [ ! -x "$XDF" ]; then
    say "creating amitools venv..."
    python3 -m venv "$D/venv" || die "venv creation failed"
    "$D/venv/bin/pip" install -q amitools || die "pip install amitools failed"
fi
LHA_BIN="$(command -v jlha || true)"
[ -z "$LHA_BIN" ] && warn "jlha not installed - .lha archive will be skipped (zip + plain tree still built)"

# ---------------------------------------------------------------- stage 1: collect inputs
MINER="$ROOT/miner/rustchain_amiga"
declare -A TOOLS=()
for t in rtcwallet rtcfetch rtctop; do
    if [ -f "$ROOT/tools/bin/$t" ]; then TOOLS[$t]="$ROOT/tools/bin/$t"; fi
done
SDK_DIR=""
[ -d "$ROOT/sdk" ] && SDK_DIR="$ROOT/sdk"

[ -f "$MINER" ] || warn "miner binary $MINER missing - pack/image will lack the miner"
for t in rtcwallet rtcfetch rtctop; do
    [ -n "${TOOLS[$t]:-}" ] || warn "tools/bin/$t not built yet - skipped (re-run assemble.sh later)"
done
[ -n "$SDK_DIR" ] || warn "sdk/ not present yet - SDK drawer skipped (re-run assemble.sh later)"

# ---------------------------------------------------------------- stage 2: tools pack
say "building pack/RustChain drawer tree"
PACK="$D/pack"
rm -rf "$PACK/RustChain"
mkdir -p "$PACK/RustChain/C" "$PACK/RustChain/S" "$PACK/RustChain/docs"

[ -f "$MINER" ] && install -m 755 "$MINER" "$PACK/RustChain/C/rustchain_amiga"
for t in "${!TOOLS[@]}"; do install -m 755 "${TOOLS[$t]}" "$PACK/RustChain/C/$t"; done

if [ -n "$SDK_DIR" ]; then
    mkdir -p "$PACK/RustChain/SDK/include" "$PACK/RustChain/SDK/lib"
    find "$SDK_DIR" -name '*.h' -not -path '*/build/*' -exec cp {} "$PACK/RustChain/SDK/include/" \; 2>/dev/null
    find "$SDK_DIR" -name 'lib*.a' -exec cp {} "$PACK/RustChain/SDK/lib/" \; 2>/dev/null
    for extra in README.md QUICKSTART.md docs examples; do
        [ -e "$SDK_DIR/$extra" ] && cp -r "$SDK_DIR/$extra" "$PACK/RustChain/SDK/" 2>/dev/null
    done
    # prune empties so we don't ship a hollow SDK drawer
    find "$PACK/RustChain/SDK" -type d -empty -delete
    [ -d "$PACK/RustChain/SDK" ] || warn "sdk/ exists but yielded no headers/libs yet"
fi

cp "$D/scripts/user-startup-RustChain" "$PACK/RustChain/S/user-startup-RustChain"
cp "$D/scripts/Install_RustChain"      "$PACK/RustChain/Install_RustChain"
cp "$D/scripts/pack-README.txt"        "$PACK/RustChain/docs/README.txt"
[ -f "$ROOT/miner/README.md" ] && cp "$ROOT/miner/README.md" "$PACK/RustChain/docs/MINER.txt"

# archives (deterministic-ish: rebuild from scratch each run)
rm -f "$PACK/rustchain-tools.zip" "$PACK/rustchain-tools.lha"
( cd "$PACK" && zip -qr rustchain-tools.zip RustChain ) || warn "zip packing failed"
if [ -n "$LHA_BIN" ]; then
    ( cd "$PACK" && "$LHA_BIN" aq rustchain-tools.lha RustChain >/dev/null 2>&1 )
    if [ -s "$PACK/rustchain-tools.lha" ]; then
        # verify round-trip
        TMP=$(mktemp -d)
        ( cd "$TMP" && "$LHA_BIN" xq "$PACK/rustchain-tools.lha" >/dev/null 2>&1 )
        if diff -r "$PACK/RustChain" "$TMP/RustChain" >/dev/null 2>&1; then
            say "rustchain-tools.lha verified (round-trip extract matches)"
        else
            warn "lha round-trip mismatch - treat .zip as authoritative"
        fi
        rm -rf "$TMP"
    else
        warn "jlha produced no archive - .zip is the fallback"
    fi
fi

# ---------------------------------------------------------------- stage 3: public HDF
say "building images/RustChainAmiga.hdf (plain FFS, boots under AROS ROM)"
HDF="$D/images/RustChainAmiga.hdf"
STAGE=$(mktemp -d)
mkdir -p "$STAGE/S" "$STAGE/C" "$STAGE/T" "$STAGE/docs"
cp "$D/scripts/startup-sequence.public" "$STAGE/S/startup-sequence"
cp "$D/scripts/image-Welcome.txt"       "$STAGE/docs/Welcome.txt"
cp "$D/scripts/pack-README.txt"         "$STAGE/docs/Tools-README.txt"
[ -f "$MINER" ] && install -m 755 "$MINER" "$STAGE/C/rustchain_amiga"
for t in "${!TOOLS[@]}"; do install -m 755 "${TOOLS[$t]}" "$STAGE/C/$t"; done
if [ -d "$PACK/RustChain/SDK" ]; then cp -r "$PACK/RustChain/SDK" "$STAGE/SDK"; fi
# keep T/ non-empty so the miner's log redirect always has its directory
echo "scratch dir for per-boot logs" > "$STAGE/T/.keep"

rm -f "$HDF"
"$XDF" "$HDF" create size=64Mi + format "RustChainAmiga" ffs || die "xdftool create/format failed"
# walk staging tree: dirs then files
( cd "$STAGE" && find . -mindepth 1 -type d | sort | sed 's|^\./||' ) | while read -r d; do
    "$XDF" "$HDF" makedir "$d" || exit 1
done || die "xdftool makedir failed"
( cd "$STAGE" && find . -type f | sort | sed 's|^\./||' ) | while read -r f; do
    "$XDF" "$HDF" write "$STAGE/$f" "$f" || exit 1
done || die "xdftool write failed"
rm -rf "$STAGE"
say "HDF contents:"
"$XDF" "$HDF" list

# ---------------------------------------------------------------- stage 4: licensed-bytes audit
say "auditing public artifacts for licensed content"
AUDIT_FAIL=0
ARTIFACTS=()
for a in "$PACK/rustchain-tools.lha" "$PACK/rustchain-tools.zip" "$HDF"; do
    [ -f "$a" ] && ARTIFACTS+=("$a")
done

# 4a. name audit: no kickstart/workbench/cloanto/amiga-os names inside artifacts
for a in "${ARTIFACTS[@]}"; do
    case "$a" in
        *.zip) LISTING=$(unzip -l "$a" 2>/dev/null) ;;
        *.lha) LISTING=$([ -n "$LHA_BIN" ] && "$LHA_BIN" l "$a" 2>/dev/null) ;;
        *.hdf) LISTING=$("$XDF" "$a" list 2>/dev/null) ;;
    esac
    if echo "$LISTING" | grep -qiE 'kickstart|cloanto|amiga-os-|workbench-[0-9]|rom\.key'; then
        echo "AUDIT FAIL (names): $a"; AUDIT_FAIL=1
    fi
done

# 4b. strings audit on the raw artifacts
for a in "${ARTIFACTS[@]}"; do
    if strings -a "$a" | grep -qiE 'cloanto|amiga forever|rom\.key'; then
        echo "AUDIT FAIL (strings): $a"; AUDIT_FAIL=1
    fi
done

# 4c. byte-sample audit: distinctive 64-byte samples from every licensed file
#     must not appear in any public artifact
if [ -d "$LICENSED" ]; then
    "$D/venv/bin/python3" - "$LICENSED" "${ARTIFACTS[@]}" <<'PYEOF' || AUDIT_FAIL=1
import sys, pathlib
lic_root, artifacts = pathlib.Path(sys.argv[1]), sys.argv[2:]
samples = []
for lf in lic_root.rglob('*'):
    if lf.suffix.lower() in ('.adf', '.hdf', '.rom') and lf.is_file():
        # pick the first 64-byte sample with real entropy (blank 0x00/0xFF
        # flash regions match anything and would false-positive)
        with open(lf, 'rb') as f:
            for off in (2048, 8192, 32768, 131072, 524288):
                f.seek(off)
                s = f.read(64)
                if len(s) == 64 and len(set(s)) >= 16:
                    samples.append((str(lf), s))
                    break
fail = False
for a in artifacts:
    blob = open(a, 'rb').read()
    for name, s in samples:
        if s in blob:
            print(f"AUDIT FAIL (licensed bytes from {name}): {a}")
            fail = True
print(f"byte-sample audit: {len(samples)} licensed samples vs {len(artifacts)} artifacts")
sys.exit(1 if fail else 0)
PYEOF
fi
[ "$AUDIT_FAIL" -eq 0 ] && say "audit clean: zero licensed bytes/names in public artifacts" \
                        || die "LICENSED CONTENT DETECTED in a public artifact - build rejected"

# ---------------------------------------------------------------- stage 5: manifest
MAN="$D/MANIFEST.txt"
{
    echo "RustChain Amiga Edition - distribution manifest"
    echo "generated: $(date -u '+%Y-%m-%d %H:%M:%SZ') by assemble.sh"
    echo ""
    echo "== public artifacts (size bytes / SHA-1) =="
    for a in "${ARTIFACTS[@]}" "$D/configs"/*.fs-uae "$D/test/rustchain-distro.fs-uae"; do
        [ -f "$a" ] || continue
        printf '%12d  %s  %s\n' "$(stat -c%s "$a")" "$(sha1sum "$a" | cut -d' ' -f1)" "${a#$D/}"
    done
    echo ""
    echo "== inputs used =="
    for a in "$MINER" "${TOOLS[@]:-}" "$ROOT/emu/roms/aros-amiga-m68k-rom.bin" "$ROOT/emu/roms/aros-amiga-m68k-ext.bin"; do
        [ -f "$a" ] || continue
        printf '%12d  %s  %s\n' "$(stat -c%s "$a")" "$(sha1sum "$a" | cut -d' ' -f1)" "$a"
    done
    echo ""
    echo "== pack drawer tree =="
    ( cd "$PACK" && find RustChain -type f -printf '%10s  %p\n' | sort -k2 )
    echo ""
    if [ "${#WARNINGS[@]}" -gt 0 ]; then
        echo "== warnings (inputs missing this run - re-run to pick them up) =="
        printf '  - %s\n' "${WARNINGS[@]}"
    else
        echo "== warnings: none, all inputs present =="
    fi
} > "$MAN"
say "manifest written to $MAN"
echo ""
cat "$MAN"
