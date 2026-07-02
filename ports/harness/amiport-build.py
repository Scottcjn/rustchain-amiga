#!/usr/bin/env python3
"""amiport-build.py - host-side build harness for the amiports tree.

Reads Portfile recipes (see ports/FORMAT.md), builds them with the
docker cross toolchain (or just collects prebuilt files), packs the
result into an .apak archive under ports/repo/packages/ and regenerates
ports/repo/index.txt.

Stdlib only. Usage:

    amiport-build.py build <portname>     build one port into the repo
    amiport-build.py build-all            build every port in the tree
    amiport-build.py index                regenerate index.txt only
    amiport-build.py serve [port]         serve ports/repo over HTTP
                                          (default port 8873, binds 0.0.0.0)

Package format (.apak, documented in ports/FORMAT.md):

    8 bytes   magic "APAK0001"
    u32 BE    file count
    per file:
      u32 BE  name length N (1..255)
      N bytes name, '/'-separated relative path, no leading slash
      u32 BE  Amiga protection bits (0 = default rwed)
      u32 BE  data size
      bytes   raw file data (no compression, no padding)

All integers are big-endian because the consumer is an m68k Amiga.
"""

import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

PORTS_DIR = Path(__file__).resolve().parent.parent
REPO_ROOT = PORTS_DIR.parent
TREE_DIR = PORTS_DIR / "tree"
REPO_DIR = PORTS_DIR / "repo"
PACKAGES_DIR = REPO_DIR / "packages"

DOCKER_IMAGE = "amigadev/crosstools:m68k-amigaos"
CROSS_CC = "m68k-amigaos-gcc"
CROSS_CFLAGS = ["-noixemul", "-m68000", "-O2", "-fomit-frame-pointer",
                "-Wall", "-Wextra"]

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
MAGIC = b"APAK0001"

# Portfile keys that may repeat
LIST_KEYS = {"source", "file", "rootfile"}
# keys every Portfile must have
REQUIRED = {"name", "version", "description", "license", "build"}


def die(msg):
    print(f"amiport-build: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_portfile(path: Path) -> dict:
    """key: value lines; '#' comments; LIST_KEYS accumulate."""
    pf = {}
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            die(f"{path}:{lineno}: expected 'key: value', got: {line}")
        key, _, value = line.partition(":")
        key, value = key.strip(), value.strip()
        if not value:
            die(f"{path}:{lineno}: empty value for '{key}'")
        if key in LIST_KEYS:
            pf.setdefault(key, []).append(value)
        elif key in pf:
            die(f"{path}:{lineno}: duplicate key '{key}'")
        else:
            pf[key] = value
    missing = REQUIRED - set(pf)
    if missing:
        die(f"{path}: missing required keys: {', '.join(sorted(missing))}")
    if not NAME_RE.match(pf["name"]):
        die(f"{path}: bad name '{pf['name']}' (lowercase, digits, dashes)")
    if "|" in pf["description"]:
        die(f"{path}: description must not contain '|'")
    if pf["build"] not in ("prebuilt", "cross-docker"):
        die(f"{path}: build must be 'prebuilt' or 'cross-docker'")
    return pf


def split_file_spec(spec: str, base: Path, what: str):
    """'src' or 'src dest' -> (abs src Path, dest name). src is relative
    to base; absolute paths and .. are rejected."""
    parts = spec.split()
    if len(parts) == 1:
        src, dest = parts[0], Path(parts[0]).name
    elif len(parts) == 2:
        src, dest = parts
    else:
        die(f"bad {what} spec (want 'src' or 'src dest'): {spec}")
    if src.startswith("/") or ".." in Path(src).parts:
        die(f"{what} src must be relative, no '..': {src}")
    if dest.startswith("/") or ".." in Path(dest).parts:
        die(f"{what} dest must be relative, no '..': {dest}")
    return base / src, dest


def cross_compile(port_dir: Path, pf: dict) -> Path:
    """Compile pf['source'] with the docker cross toolchain inside the
    port dir. Returns the path of the produced hunk binary."""
    sources = pf.get("source")
    if not sources:
        die(f"{pf['name']}: build 'cross-docker' needs at least one source:")
    for s in sources:
        if s.startswith("/") or ".." in Path(s).parts:
            die(f"{pf['name']}: source must be relative, no '..': {s}")
        if not (port_dir / s).is_file():
            die(f"{pf['name']}: source not found: {port_dir / s}")

    want_sha = pf.get("source-sha1")
    if want_sha:
        got = hashlib.sha1((port_dir / sources[0]).read_bytes()).hexdigest()
        if got != want_sha.lower():
            die(f"{pf['name']}: source-sha1 mismatch on {sources[0]}\n"
                f"  want {want_sha}\n  got  {got}")

    program = pf.get("program", pf["name"])
    extra = pf.get("cflags", "").split()
    cmd = ["docker", "run", "--rm",
           "-v", f"{port_dir}:/work", "-w", "/work",
           DOCKER_IMAGE, CROSS_CC] + CROSS_CFLAGS + extra + \
          ["-o", program] + sources
    print(f"  cross-compiling: {CROSS_CC} {' '.join(CROSS_CFLAGS + extra)} "
          f"-o {program} {' '.join(sources)}")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.stdout.strip():
        print(proc.stdout, end="")
    if proc.stderr.strip():
        print(proc.stderr, end="", file=sys.stderr)
    if proc.returncode != 0:
        die(f"{pf['name']}: cross compile failed (exit {proc.returncode})")
    out = port_dir / program
    if not out.is_file():
        die(f"{pf['name']}: compiler exited 0 but {out} is missing")
    # docker may leave the output root-owned; fix silently if possible
    subprocess.run(["docker", "run", "--rm", "-v", f"{port_dir}:/work",
                    "-w", "/work", DOCKER_IMAGE,
                    "chmod", "0666", program],
                   capture_output=True)
    return out


def collect_members(port_dir: Path, pf: dict):
    """Return [(dest_name, bytes)] for the archive, in a stable order."""
    members = []

    if pf["build"] == "cross-docker":
        binary = cross_compile(port_dir, pf)
        members.append((binary.name, binary.read_bytes()))

    for spec in pf.get("file", []):
        src, dest = split_file_spec(spec, port_dir, "file")
        if not src.is_file():
            die(f"{pf['name']}: file not found: {src}")
        members.append((dest, src.read_bytes()))

    for spec in pf.get("rootfile", []):
        src, dest = split_file_spec(spec, REPO_ROOT, "rootfile")
        if not src.is_file():
            die(f"{pf['name']}: rootfile not found: {src}")
        members.append((dest, src.read_bytes()))

    if not members:
        die(f"{pf['name']}: nothing to package (no source/file/rootfile)")
    seen = set()
    for dest, _ in members:
        if dest in seen:
            die(f"{pf['name']}: duplicate archive member '{dest}'")
        seen.add(dest)
    return members


def write_apak(members, out_path: Path):
    """Pack members into an .apak archive (protection bits all 0)."""
    with out_path.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack(">I", len(members)))
        for dest, data in members:
            name = dest.encode("ascii")
            if not (1 <= len(name) <= 255):
                die(f"member name length out of range: {dest}")
            f.write(struct.pack(">I", len(name)))
            f.write(name)
            f.write(struct.pack(">I", 0))          # protection: default rwed
            f.write(struct.pack(">I", len(data)))
            f.write(data)


def build_port(name: str):
    port_dir = TREE_DIR / name
    portfile = port_dir / "Portfile"
    if not portfile.is_file():
        die(f"no such port: {name} ({portfile} missing)")
    pf = parse_portfile(portfile)
    if pf["name"] != name:
        die(f"{portfile}: name '{pf['name']}' does not match directory")

    print(f"building port {name} {pf['version']} ({pf['build']})")
    members = collect_members(port_dir, pf)

    PACKAGES_DIR.mkdir(parents=True, exist_ok=True)
    archive = PACKAGES_DIR / f"{name}-{pf['version']}.apak"
    write_apak(members, archive)
    sha1 = hashlib.sha1(archive.read_bytes()).hexdigest()
    print(f"  packaged {archive.name}: {len(members)} file(s), "
          f"{archive.stat().st_size} bytes, sha1 {sha1}")
    return pf


def regen_index():
    """index.txt from every Portfile whose archive exists on disk."""
    lines = ["# amiports repo index"
             " - name|version|archive|sha1|size|license|description"]
    count = 0
    for portfile in sorted(TREE_DIR.glob("*/Portfile")):
        pf = parse_portfile(portfile)
        archive = PACKAGES_DIR / f"{pf['name']}-{pf['version']}.apak"
        if not archive.is_file():
            print(f"  index: skipping {pf['name']} (not built)")
            continue
        sha1 = hashlib.sha1(archive.read_bytes()).hexdigest()
        lines.append("|".join([
            pf["name"], pf["version"], archive.name, sha1,
            str(archive.stat().st_size), pf["license"], pf["description"],
        ]))
        count += 1
    REPO_DIR.mkdir(parents=True, exist_ok=True)
    (REPO_DIR / "index.txt").write_text("\n".join(lines) + "\n")
    print(f"  index.txt: {count} package(s)")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 5
    cmd = argv[1]
    if cmd == "build" and len(argv) == 3:
        build_port(argv[2])
        regen_index()
    elif cmd == "build-all":
        for portfile in sorted(TREE_DIR.glob("*/Portfile")):
            build_port(portfile.parent.name)
        regen_index()
    elif cmd == "index":
        regen_index()
    elif cmd == "serve":
        port = int(argv[2]) if len(argv) > 2 else 8873
        subprocess.run([sys.executable, "-m", "http.server", str(port),
                        "--bind", "0.0.0.0", "--directory", str(REPO_DIR)])
    else:
        print(__doc__)
        return 5
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
