# Boing

No Amiga repo is complete without the Boing Ball.

`boing.c` is the classic Boing Ball as native m68k AmigaOS code: it opens a
custom screen, draws the purple grid, renders a red-and-white checkered sphere
with a proper curved (spherical) checker, and drops a shadow on the floor.
Pure `graphics.library` + `intuition.library`, C89, built with the same bebbo
`m68k-amigaos` cross toolchain (`-m68020`) as the rest of this repo.

![Boing Ball, rendered by native m68k code on the Amiga](screenshots/boing-ball.png)

Rendered on the open-source AROS m68k ROM in FS-UAE. The checker curves toward
the edges because each pixel's horizontal cell is scaled by the sphere's depth
(`z` from an integer square root), which is what gives it the ball look instead
of a flat disc.

## Build

```
docker run --rm -v "$PWD/..":/work -w /work/boing \
  amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer -Wall -o boing boing.c
```

## Run (in-emulator)

```
cd test && fs-uae boing.fs-uae
```
