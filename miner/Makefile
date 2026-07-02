# RustChain Amiga miner build
# Cross-compiles via the amigadev/crosstools docker image (bebbo gcc 6.5.0b).
# bebbo/amiga-gcc no longer exists on Docker Hub; this is the maintained build of it.

DOCKER_IMAGE = amigadev/crosstools:m68k-amigaos
DOCKER_RUN   = docker run --rm -v $(CURDIR):/work -w /work $(DOCKER_IMAGE)
AMIGA_CC     = m68k-amigaos-gcc
AMIGA_CFLAGS = -noixemul -m68000 -O2 -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter

HOST_CC      = gcc
HOST_CFLAGS  = -DHOST_TEST -O2 -Wall -Wextra

SRC = rustchain_amiga_miner.c

all: rustchain_amiga

rustchain_amiga: $(SRC)
	$(DOCKER_RUN) $(AMIGA_CC) $(AMIGA_CFLAGS) -o $@ $(SRC)
	file $@

host-test: host_test host-test32
	./host_test

host_test: $(SRC)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(SRC)

# 32-bit ILP32 run (long = 4 bytes, same as m68k). Catches int-promotion
# and libgcc 64-bit-op bugs the x86_64 test cannot see. Plain gcc -m32
# lacks multilib libs on this box, the i386 docker image is the
# no-system-changes way to get a true 32-bit userland.
HOST32_IMAGE = i386/gcc
host-test32: $(SRC)
	docker run --rm -v $(CURDIR):/work -w /work $(HOST32_IMAGE) \
	  sh -c "gcc $(HOST_CFLAGS) -o host_test32 $(SRC) && ./host_test32"

clean:
	rm -f rustchain_amiga host_test host_test32

.PHONY: all host-test host-test32 clean
