# amiport client build
# Same toolchain rules as miner/ and tools/: cross-compile with the
# amigadev/crosstools docker image, -m68000 so it runs on every Amiga.
# The host build (-DHOST_TEST) uses real POSIX sockets, so the full
# list/install flow can be exercised on Linux against the same repo.

DOCKER_IMAGE = amigadev/crosstools:m68k-amigaos
DOCKER_RUN   = docker run --rm -v $(CURDIR):/work -w /work $(DOCKER_IMAGE)
AMIGA_CC     = m68k-amigaos-gcc
AMIGA_CFLAGS = -noixemul -m68000 -O2 -fomit-frame-pointer -Wall -Wextra

HOST_CC      = gcc
HOST_CFLAGS  = -DHOST_TEST -O2 -Wall -Wextra

SRC  = amiport.c vendor/rtc_common.c
HDRS = vendor/rtc_common.h

all: amiport

amiport: $(SRC) $(HDRS)
	$(DOCKER_RUN) $(AMIGA_CC) $(AMIGA_CFLAGS) -o $@ $(SRC)
	file $@

host_amiport: $(SRC) $(HDRS)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(SRC)

# 32-bit ILP32 host build (long = 4 bytes like m68k); catches
# int-promotion bugs the x86_64 build cannot see.
HOST32_IMAGE = i386/gcc
host32_amiport: $(SRC) $(HDRS)
	docker run --rm -v $(CURDIR):/work -w /work $(HOST32_IMAGE) \
	  gcc $(HOST_CFLAGS) -static -o $@ $(SRC)

clean:
	rm -f amiport host_amiport host32_amiport

.PHONY: all clean
