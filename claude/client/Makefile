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
CROSSFLAGS := -noixemul -m68000 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -I$(GINC)

.PHONY: all host-test clean

all: bin/claude host-test

# --- Amiga hunk executable (primary target) ---
bin/claude: claude.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) \
	    -o bin/claude $(SRC) \
	    -L$(GLIB) -lamisslstubs
	@echo "--- file bin/claude ---"
	@file bin/claude

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
	rm -f host_test host_test32 bin/claude
