# RustChain Amiga tools build (rtcwallet, rtcfetch, rtctop)
# Same toolchain rules as ../miner/Makefile: cross-compile with the
# amigadev/crosstools docker image (bebbo gcc 6.5.0b, bebbo/amiga-gcc is
# gone from Docker Hub), -m68000 so the binaries run on every Amiga.

DOCKER_IMAGE = amigadev/crosstools:m68k-amigaos
DOCKER_RUN   = docker run --rm -v $(CURDIR):/work -w /work $(DOCKER_IMAGE)
AMIGA_CC     = m68k-amigaos-gcc
AMIGA_CFLAGS = -noixemul -m68000 -O2 -fomit-frame-pointer -Wall -Wextra -Icommon

HOST_CC      = gcc
HOST_CFLAGS  = -DHOST_TEST -O2 -Wall -Wextra -Icommon

COMMON  = common/rtc_common.c
HDRS    = common/rtc_common.h
TOOLS   = rtcwallet rtcfetch rtctop

all: $(addprefix bin/,$(TOOLS))

bin/%: src/%.c $(COMMON) $(HDRS)
	$(DOCKER_RUN) $(AMIGA_CC) $(AMIGA_CFLAGS) -o $@ src/$*.c $(COMMON)
	file $@

# Host tests: same sources with -DHOST_TEST, run against the real node
# responses captured in testdata/ (JSON parsing, table rendering, url
# and HTTP formatting). Run from this directory.
host-test: $(addprefix host_,$(TOOLS))
	./host_rtcwallet
	./host_rtcfetch
	./host_rtctop

host_%: src/%.c $(COMMON) $(HDRS)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ src/$*.c $(COMMON)

# 32-bit ILP32 run (long = 4 bytes, same as m68k). Catches int-promotion
# bugs the x86_64 run cannot see. Same docker trick as the miner: this
# box has no gcc multilib, i386/gcc gives a true 32-bit userland.
HOST32_IMAGE = i386/gcc
host-test32: $(patsubst %,src/%.c,$(TOOLS)) $(COMMON) $(HDRS)
	docker run --rm -v $(CURDIR):/work -w /work $(HOST32_IMAGE) sh -c '\
	  set -e; \
	  for t in $(TOOLS); do \
	    gcc $(HOST_CFLAGS) -o host32_$$t src/$$t.c $(COMMON); \
	    ./host32_$$t; \
	  done'

# refresh the captured node responses (read-only GETs).
# NOTE: the host tests lock exact values from these captures (epoch,
# slot, balance). After refreshing, update the constants in src/*.c
# host-test sections to match.
testdata:
	curl -s -m 15 "http://50.28.86.131:8088/api/miners" -o testdata/miners.json
	curl -s -m 10 "http://50.28.86.131:8088/epoch" -o testdata/epoch.json
	curl -s -m 10 "http://50.28.86.131:8088/wallet/balance?miner_id=dual-g4-125" -o testdata/balance.json
	curl -s -m 10 "http://50.28.86.131:8088/health" -o testdata/health.json

clean:
	rm -f bin/rtcwallet bin/rtcfetch bin/rtctop
	rm -f host_rtcwallet host_rtcfetch host_rtctop
	rm -f host32_rtcwallet host32_rtcfetch host32_rtctop

.PHONY: all host-test host-test32 testdata clean
