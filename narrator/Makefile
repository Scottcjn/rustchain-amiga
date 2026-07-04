# Cross-compile say for AmigaOS m68k using the amigadev/crosstools docker image.
# Note: -m68020 on purpose. bebbo gcc miscompiles __mulsi3/atoi at -m68000 and hangs.

REPO   = /home/scott/rustchain-amiga
DOCKER = docker run --rm -v $(REPO):/work -w /work/narrator amigadev/crosstools:m68k-amigaos
CC     = m68k-amigaos-gcc
CFLAGS = -noixemul -m68020 -O2 -fomit-frame-pointer -Wall -Wextra

all: bin/say

bin/say: say.c
	mkdir -p bin
	$(DOCKER) $(CC) $(CFLAGS) -o bin/say say.c
	file bin/say

clean:
	rm -f bin/say

.PHONY: all clean
