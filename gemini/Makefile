# Makefile - Gemini protocol client/browser for AmigaOS
#
# Targets:
#   make host-test    native + i386 (ILP32) self-test of the pure logic
#                     (URL parsing, status-line parsing, relative-URL
#                     resolution, text/gemini rendering) -- no network
#   make bin/gemini   Amiga hunk exe (docker cross toolchain + AmiSSL)
#   make bin/gemini-proxy   Amiga hunk exe, no AmiSSL linked (--proxy only)
#   make all          both
#   make clean
#
# Modeled on claude/client/Makefile. The Amiga build cross-compiles with the
# amigadev/crosstools m68k image and links against the AmiSSL SDK that ships
# in this repo. See README.md.

REPO    := $(abspath ..)
DOCKER  := docker run --rm -v $(REPO):/work -w /work/gemini \
             amigadev/crosstools:m68k-amigaos
GINC    := /work/python/src/amissl-sdk/AmiSSL/Developer/include
GLIB    := /work/python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3

# -m68020: this client needs a full AmiSSL + bsdsocket AROS (A4000/040 class,
# never a bare 68000). -m68020 emits native 32-bit MULU.L/MULS.L instead of
# bebbo's __mulsi3 helper, which HANGS on any runtime-variable 32-bit
# multiply at -m68000 (the proven bebbo miscompile; see the file header in
# gemini.c and claude/client/claude.c). Matches every other native port in
# this repo, which are all on -m68020 for the same reason.
CROSSFLAGS := -noixemul -m68020 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -I$(GINC)

SRC := gemini.c vendor/rtc_common.c

.PHONY: all host-test clean

all: bin/gemini bin/gemini-proxy host-test

# --- Amiga hunk executable (primary target, AmiSSL direct TLS + proxy) ---
bin/gemini: gemini.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) \
	    -o bin/gemini $(SRC) \
	    -L$(GLIB) -lamisslstubs
	@echo "--- file bin/gemini ---"
	@file bin/gemini

# --- proxy-only build (no AmiSSL): for a bare bsdsocket AROS with no TLS
#     library. Smaller, links no TLS. Run with --proxy host:port.
#     -DNO_AMISSL drops the AmiSSL transport and its stubs. ---
bin/gemini-proxy: gemini.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer \
	    -Wall -Wextra -Wno-unused-parameter -DNO_AMISSL \
	    -o bin/gemini-proxy $(SRC)
	@echo "--- file bin/gemini-proxy ---"
	@file bin/gemini-proxy

# --- host self-test (native + 32-bit ILP32) ---
host-test: host_test host_test32
	@echo "== native =="
	./host_test
	@echo "== i386 (ILP32) =="
	./host_test32 || true

host_test: gemini.c
	cc -DHOST_TEST -Wall -Wextra -O2 -o host_test gemini.c

host_test32: gemini.c
	cc -DHOST_TEST -m32 -Wall -Wextra -O2 -o host_test32 gemini.c \
	  || echo "(-m32 unavailable on this host; native test is authoritative)"

clean:
	rm -f host_test host_test32 bin/gemini bin/gemini-proxy
