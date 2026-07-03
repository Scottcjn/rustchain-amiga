# Makefile - Claude on the Amiga client
#
# Targets:
#   make host-test   native + i386 self-test of the JSON/message logic
#   make claude      Amiga hunk exe (docker cross toolchain + AmiSSL)
#   make all         both
#   make clean
#
# The Amiga build cross-compiles with the amigadev/crosstools m68k image and
# links against the AmiSSL SDK that ships in this repo. See ../README.md.

REPO    := $(abspath ../..)
AMISSL  := $(REPO)/python/src/amissl-sdk/AmiSSL/Developer
DOCKER  := docker run --rm -v $(REPO):/work -w /work/claude/client \
             amigadev/crosstools:m68k-amigaos
GINC    := /work/python/src/amissl-sdk/AmiSSL/Developer/include
GLIB    := /work/python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3

SRC     := claude.c vendor/rtc_common.c
# -m68020: the client requires a full AmiSSL + bsdsocket AROS (A4000/040 class,
# never a bare 68000), and -m68020 emits native 32-bit MULU.L/MULS.L instead of
# bebbo's __mulsi3 helper, which HANGS on this target -- the proven bebbo
# -m68000 miscompile (also the 64-bit-shift bug). Matches the python/ and java/
# ports, which are on -m68020 for the same reason.
CROSSFLAGS := -noixemul -m68020 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -I$(GINC)

.PHONY: all host-test clean

all: bin/claude bin/claude-proxy host-test

# --- Amiga hunk executable (primary target, AmiSSL direct HTTPS + proxy) ---
bin/claude: claude.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) \
	    -o bin/claude $(SRC) \
	    -L$(GLIB) -lamisslstubs
	@echo "--- file bin/claude ---"
	@file bin/claude

# --- proxy-only build (no AmiSSL): for bare-ROM AROS with only bsdsocket.
#     Smaller, links no TLS. Run with --proxy host:port. -DNO_AMISSL drops the
#     AmiSSL transport and its stubs. ---
bin/claude-proxy: claude.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer \
	    -Wall -Wextra -Wno-unused-parameter -DNO_AMISSL \
	    -o bin/claude-proxy claude.c vendor/rtc_common.c
	@echo "--- file bin/claude-proxy ---"
	@file bin/claude-proxy

# --- host self-test (native + 32-bit ILP32) ---
host-test: host_test host_test32
	@echo "== native =="
	./host_test
	@echo "== i386 (ILP32) =="
	./host_test32 || true

host_test: claude.c vendor/rtc_common.c vendor/rtc_common.h
	cc -DHOST_TEST -Wall -Wextra -O2 -o host_test claude.c vendor/rtc_common.c

host_test32: claude.c vendor/rtc_common.c vendor/rtc_common.h
	cc -DHOST_TEST -m32 -Wall -Wextra -O2 -o host_test32 claude.c vendor/rtc_common.c \
	  || echo "(-m32 unavailable on this host; native test is authoritative)"

clean:
	rm -f host_test host_test32 bin/claude bin/claude-proxy
