# amiports formats

Three formats make up the system: the Portfile recipe, the .apak package
archive, and the repo index. All three are deliberately simple enough to
parse with a Python stdlib script on the host, a C89 program on a 68000,
and eyeballs.

## 1. Portfile (recipe)

One `Portfile` per port at `ports/tree/<portname>/Portfile`. Plain text,
`key: value` lines, `#` comments, blank lines ignored. Keys:

| Key | Required | Meaning |
|-----|----------|---------|
| `name` | yes | package name, `[a-z0-9-]`, must equal the directory name |
| `version` | yes | version string, used in the archive filename |
| `description` | yes | one line, shown by `amiport list`; no `\|` allowed |
| `license` | yes | SPDX-ish license tag recorded in the index (MIT, GPL-2.0, ...) |
| `build` | yes | `cross-docker` or `prebuilt` |
| `source` | cross-docker | .c file relative to the port dir; repeatable |
| `source-sha1` | no | expected SHA-1 of the first `source` file (integrity pin) |
| `program` | no | output binary name (default: port name) |
| `cflags` | no | extra compiler flags appended to the standard set |
| `file` | no | `src [dest]` packaged as-is, src relative to the port dir; repeatable |
| `rootfile` | no | `src [dest]` packaged as-is, src relative to the repo root `~/rustchain-amiga/` (for artifacts built by the other workstreams); repeatable |

Build kinds:

- `cross-docker`: the harness compiles `source` inside
  `amigadev/crosstools:m68k-amigaos` with
  `m68k-amigaos-gcc -noixemul -m68000 -O2 -fomit-frame-pointer -Wall -Wextra`
  (plus `cflags`) and packages the resulting hunk binary. `-m68000` keeps
  every package runnable on every Amiga.
- `prebuilt`: no compile step; the harness just packages the listed
  `file`/`rootfile` entries.

Absolute paths and `..` are rejected everywhere, in both src and dest.

Example (source-built):

    name: hello-amiports
    version: 1.0
    description: proof-of-build hello program, cross-compiled from source
    license: MIT
    build: cross-docker
    source: hello.c
    program: hello

Example (prebuilt):

    name: rustchain-tools
    version: 1.0
    description: RustChain tools for AmigaOS - rtcwallet, rtcfetch, rtctop and the attestation miner
    license: MIT
    build: prebuilt
    rootfile: tools/bin/rtcwallet rtcwallet
    rootfile: miner/rustchain_amiga rustchain_amiga

## 2. .apak (package archive)

Why not .lha: lha is the Amiga-native archiver and this box can even
create .lha (jlha-utils is installed), but the *extractor* has to run on
the Amiga, and the bare AROS ROM boot ships no lha binary. Depending on
one, or writing an LZH decompressor in C89 under the m68k compiler
constraints (no 64-bit shifts at -m68000, small stacks), is exactly the
kind of risk a package manager's trust root should not carry. So amiports
uses its own uncompressed container that extracts in ~60 lines of C89.
.lha remains the human-facing format for the distro/ tools pack; a later
harness version can emit both.

Layout (all integers big-endian u32 - m68k native byte order, the guest
reads them with plain shifts):

    offset  size  field
    0       8     magic "APAK0001"
    8       4     file count (1..1000)
    then, per file, back to back:
            4     name length N (1..255)
            N     name: '/'-separated relative path, ASCII,
                  no leading '/', no ':', no '\', no '..' segments
            4     Amiga protection bits (0 = default rwed; the client
                  calls SetProtection() only when nonzero)
            4     data size in bytes (max 8 MB per member)
            size  raw file data, no compression, no padding

No trailer. Truncation is caught because every member declares its size,
and the whole archive is SHA-1 verified against the index before the
first byte is extracted.

## 3. Repo layout and index

    ports/repo/
      index.txt              the index
      packages/
        <name>-<version>.apak

`index.txt`: one package per line, pipe-separated, `#` lines are
comments:

    name|version|archive|sha1|size|license|description

- `archive` is the filename under `packages/`
- `sha1` is the lowercase hex SHA-1 of the archive file
- `size` is the archive size in bytes
- `description` is last so it may contain anything except `|` and newline

The whole index is fetched in one HTTP/1.0 GET (64 KB client cap, plenty
for hundreds of ports). Serving is any static HTTP server:
`python3 -m http.server` for dev, node 1 nginx or GitHub releases later.

## 4. installed.db (on-Amiga registration)

`<prefix>/installed.db` (default prefix `SYS:amiports`), one line
appended per successful install:

    name|version|archive_sha1|filecount

Files land in `<prefix>/<name>/`. Reinstalling appends a new line; the
newest line for a name is the current one.
