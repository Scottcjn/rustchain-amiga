# MicroPython for classic m68k AmigaOS

Phase 3 of RustChain Amiga Edition: a working MicroPython (`upython`) built as a
native AmigaOS hunk executable for 68020+ machines, using the same
`amigadev/crosstools` docker toolchain as the miner and SDK.

## What is in this directory

This directory holds only the port's own code:

| Path | What |
|------|------|
| `port/amiga/` | The Amiga port glue: `main.c`, `Makefile`, `mpconfigport.h`, `mphalport.h`, `qstrdefsport.h` |
| `patches/` | Patches applied to the upstream port for the crosstools NDK |
| `build.sh` | Full build (fetches deps into `src/`, runs the cross build, emits `bin/upython`) |
| `build-min.sh` | Minimal build variant |

The large upstream sources live under `src/` and are **not** committed here (see
Dependencies). Build outputs (`bin/`) and test artifacts (`test/`) are ignored.

## Dependencies (fetched into `src/`, not vendored)

`build.sh` expects these cloned into `src/` before it runs. It prints the exact
clone command if they are missing.

| Dependency | Source | License |
|------------|--------|---------|
| MicroPython Amiga port | https://github.com/OoZe1911/micropython-amiga-port | MIT |
| MicroPython (upstream) | https://github.com/micropython/micropython | MIT (Damien P. George) |
| AmiSSL SDK 5.27 | https://github.com/jens-maus/AmiSSL | open source (see its release) |

These are pulled at build time rather than copied into this repo, so their code
stays in their own projects under their own licenses.

## Build

```sh
# from this directory, with docker available
./build.sh          # clones missing deps into src/, cross-compiles
# result: bin/upython  (AmigaOS m68k hunk executable)
```

## License

The port's own code in this directory (`port/amiga/`, `patches/`, the build
scripts) is licensed under the GNU AGPLv3, the same as the rest of this repo
(see the top-level `LICENSE`).

The dependencies above are the work of their respective authors and remain under
their own licenses (MIT for MicroPython and the Amiga port, open source for
AmiSSL). Nothing in this directory relicenses them; they are fetched from
upstream, not redistributed here.
