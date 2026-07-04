# say - make the Amiga speak

A small native AmigaOS (m68k) CLI that does the classic SAY trick from C:
it runs your text through `translator.library` to get phonemes, then feeds
the phonemes to `narrator.device` so the Amiga speaks them through its own
audio channels.

## Usage

```
say "hello world, this is your amiga speaking"
echo "hello from a pipe" | say
say --dry-run "hello world"
```

With no text arguments it reads stdin. `--dry-run` prints the phoneme
translation instead of speaking, so you can verify the translator path
without audio hardware.

Example dry run output:

```
> say --dry-run hello world
/HEHLOW WER1LD.
```

## How it works

1. `OpenLibrary("translator.library", 0)` and `Translate()` convert the
   English text into a phonetic string.
2. `CreateMsgPort()` + `CreateIORequest()` build a `struct narrator_rb`,
   `OpenDevice("narrator.device", 0, ...)` opens the speech device.
3. A `CMD_WRITE` is sent with `io_Data` pointing at the phoneme string and
   `io_Length` set to its length. Voice settings are the stock defaults from
   `devices/narrator.h`: rate 150 wpm, pitch 110 Hz, natural contours, male
   voice, volume 64, 22200 Hz sampling. Channel allocation maps 3, 5, 10, 12
   ask for one left plus one right audio channel.
4. Cleanup runs in the correct order: AbortIO/WaitIO if a request is still
   pending, CloseDevice, DeleteIORequest, DeleteMsgPort, CloseLibrary.

## Requirements on the Amiga side

`narrator.device` and `translator.library` must be present (Workbench 1.x
through 2.x shipped them; on 3.x installs copy them in from a 2.x disk or an
Aminet mirror: `translator.library` goes in `LIBS:`, `narrator.device` in
`DEVS:`). Audio output needs FS-UAE with sound enabled, or real hardware.

## Building

Cross-compiled with the bebbo toolchain in docker:

```
make
```

which runs:

```
docker run --rm -v /home/scott/rustchain-amiga:/work -w /work/narrator \
  amigadev/crosstools:m68k-amigaos \
  m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer \
  -Wall -Wextra -o bin/say say.c
```

`file bin/say` must report "AmigaOS loadseg()ble executable/binary".
Note `-m68020`, not `-m68000`: bebbo gcc miscompiles some libnix helpers
at -m68000 and the binary hangs.

## Piping Claude replies into the speaker

The Claude-on-Amiga client lives in `../claude/`. To have the Amiga read a
reply out loud, capture the reply into a file and feed it to `say`:

```
claude "give me a one line greeting" > RAM:reply.txt
say < RAM:reply.txt
```

or conceptually, on a shell with pipes mounted:

```
claude "give me a one line greeting" | say
```

Classic AmigaShell 3.1 has no native `|`, so the RAM: file redirect is the
reliable route there.

## Testing status

The binary cross-compiles clean (-Wall -Wextra, no warnings) and is a valid
AmigaOS hunk executable. The `--dry-run` translator path and the actual
speech output need to be checked in FS-UAE with audio (or on real hardware);
narrator.device I/O cannot be exercised on the build host.
