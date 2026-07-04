# Makefile - Mastodon / Fediverse client for classic AmigaOS (m68k)
#
# Targets:
#   make host-test          native self-test of JSON/HTML/request-builder
#                            logic (-DHOST_TEST, no network, no Amiga)
#   make host-proxy-demo    native build of the REAL client (-DHOST_PROXY_DEMO)
#                            with POSIX sockets instead of bsdsocket, so the
#                            --proxy path can be run against
#                            proxy/mastodon_amiga_proxy.py on this machine
#   make bin/mastodon       Amiga m68k hunk exe (docker cross toolchain +
#                            AmiSSL), both transports built in
#   make all                all of the above
#   make clean
#
# mastodon.c is a single self-contained translation unit (no vendor/ split),
# so the cross build is exactly the one-liner in the task brief. Cross
# compiles with the amigadev/crosstools m68k image and links against the
# AmiSSL SDK vendored in this repo.

REPO    := $(abspath ..)
AMISSL  := $(REPO)/python/src/amissl-sdk/AmiSSL/Developer
DOCKER  := docker run --rm -v $(REPO):/work -w /work/mastodon \
             amigadev/crosstools:m68k-amigaos
GINC    := /work/python/src/amissl-sdk/AmiSSL/Developer/include
GLIB    := /work/python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3

# -m68020, NOT -m68000: bebbo gcc miscompiles the variable-multiply helper
# (__mulsi3) and 64-bit shifts at -m68000, which HANGS the binary at runtime
# on real hardware/FS-UAE. See claude/client/Makefile for the same rule and
# mastodon.c's rc_strtoul comment for the m68k-safe integer parsing this
# client uses instead of atoi/strtol.
CROSSFLAGS := -noixemul -m68020 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -I$(GINC)

.PHONY: all host-test host-proxy-demo clean

all: bin/mastodon host-test host-proxy-demo

# --- Amiga hunk executable (AmiSSL direct HTTPS + proxy fallback) ---
bin/mastodon: mastodon.c
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) \
	    -o bin/mastodon mastodon.c \
	    -L$(GLIB) -lamisslstubs
	@echo "--- file bin/mastodon ---"
	@file bin/mastodon

# --- host self-test: pure JSON/HTML/request-builder logic, no network ---
host-test: host_test
	./host_test

host_test: mastodon.c
	cc -DHOST_TEST -Wall -Wextra -O2 -o host_test mastodon.c

# --- host proxy demo: the real client (do_timeline/do_post/proxy_request)
#     compiled natively with POSIX sockets, so it can drive
#     proxy/mastodon_amiga_proxy.py end to end without an Amiga or emulator.
#     It only supports --proxy (there is no AmiSSL on Linux). ---
host-proxy-demo: host_mastodon_proxydemo
	@echo "built host_mastodon_proxydemo -- see ../README.md for how to run"
	@echo "it against proxy/mastodon_amiga_proxy.py --mock"

host_mastodon_proxydemo: mastodon.c
	cc -DHOST_PROXY_DEMO -Wall -Wextra -O2 -o host_mastodon_proxydemo mastodon.c

clean:
	rm -f host_test host_mastodon_proxydemo bin/mastodon
