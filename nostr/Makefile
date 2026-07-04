# Makefile - native Nostr client for AmigaOS (m68k)
#
# Targets:
#   make host-test   native (and i386/ILP32) self-test of the pure logic
#                    (sha1, sha256, base64, WebSocket framing, Nostr JSON,
#                     event serialization + id)
#   make nostr       Amiga hunk exe (docker cross toolchain + AmiSSL, wss+ws)
#   make nostr-proxy Amiga hunk exe without AmiSSL (ws:// / --proxy only)
#   make all         host-test + both Amiga binaries
#   make clean
#
# The Amiga build cross-compiles with the amigadev/crosstools m68k image and
# links against the AmiSSL SDK that ships in this repo. See README.md.

REPO    := $(abspath ..)
AMISSL  := $(REPO)/python/src/amissl-sdk/AmiSSL/Developer
DOCKER  := docker run --rm -v $(REPO):/work -w /work/nostr \
             amigadev/crosstools:m68k-amigaos
GINC    := /work/python/src/amissl-sdk/AmiSSL/Developer/include
GLIB    := /work/python/src/amissl-sdk/AmiSSL/Developer/lib/AmigaOS3

SRC     := nostr.c vendor/rtc_common.c

# -m68020 (NOT -m68000): bebbo emits __mulsi3 for runtime 32-bit multiplies at
# -m68000, which HANGS on this target (same proven bug as atoi/64-bit shift).
# -m68020 emits native MULU.L. Matches the claude/, python/ and java/ ports.
CROSSFLAGS := -noixemul -m68020 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter -I$(GINC)

.PHONY: all host-test host-net demo clean

all: bin/nostr bin/nostr-proxy host-test

# --- Amiga hunk executable (AmiSSL direct TLS for wss:// + plain ws://) ---
bin/nostr: nostr.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) \
	    -o bin/nostr $(SRC) \
	    -L$(GLIB) -lamisslstubs
	@echo "--- file bin/nostr ---"
	@file bin/nostr

# --- proxy/ws-only build (no AmiSSL linked): smaller, for bare-ROM AROS.
#     Run with a ws:// relay or --proxy host:port. ---
bin/nostr-proxy: nostr.c vendor/rtc_common.c vendor/rtc_common.h
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer \
	    -Wall -Wextra -Wno-unused-parameter -DNO_AMISSL \
	    -o bin/nostr-proxy nostr.c vendor/rtc_common.c
	@echo "--- file bin/nostr-proxy ---"
	@file bin/nostr-proxy

# --- host self-test (native + 32-bit ILP32 to mirror the m68k ABI) ---
host-test: host_test host_test32
	@echo "== native =="
	./host_test
	@echo "== i386 (ILP32) =="
	./host_test32 || true

host_test: nostr.c
	cc -DHOST_TEST -Wall -Wextra -O2 -o host_test nostr.c

host_test32: nostr.c
	cc -DHOST_TEST -m32 -Wall -Wextra -O2 -o host_test32 nostr.c \
	  || echo "(-m32 unavailable on this host; native test is authoritative)"

# --- host-native build of the SAME networking code (POSIX sockets, no AmiSSL).
#     Runs the read path on the host so it can be driven against a mock relay.
#     plain ws:// and --proxy only. ---
host_net: nostr.c
	cc -DHOST_NET -Wall -Wextra -Wno-unused-parameter -O2 -o host_net nostr.c

host-net: host_net

# --- end-to-end read-path demo: start the hand-rolled mock relay, point the
#     host-native client at it, print the notes it receives. ---
demo: host_net
	@python3 tools/mock_relay.py 7000 > /tmp/nostr_mock_relay.log 2>&1 & \
	  RP=$$!; sleep 0.6; \
	  ./host_net -v --eose-exit ws://127.0.0.1:7000/; \
	  sleep 0.3; echo "--- relay log ---"; cat /tmp/nostr_mock_relay.log; \
	  kill $$RP 2>/dev/null || true

clean:
	rm -f host_test host_test32 host_net bin/nostr bin/nostr-proxy
