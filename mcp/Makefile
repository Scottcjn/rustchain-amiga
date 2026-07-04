# Makefile - Amiga MCP server
#
# Targets:
#   make host-test    native self-test of the JSON-RPC / MCP protocol logic
#   make mcp          Amiga hunk exe (docker cross toolchain, m68k-amigaos)
#   make all          both
#   make clean
#
# The Amiga build cross-compiles with the amigadev/crosstools m68k image. This
# server is stdio only (no sockets, no AmiSSL), so it links nothing extra.
# See README.md.

REPO   := $(abspath ..)
DOCKER := docker run --rm -v $(REPO):/work -w /work/mcp \
            amigadev/crosstools:m68k-amigaos

# -m68020 (NOT -m68000): bebbo/libnix miscompiles __mulsi3 / atoi at -m68000
# and HANGS on this target. Every other port in this repo builds at -m68020 for
# the same reason. This server never touches the multiply/atoi path, but we
# hold the line so the whole repo stays on one proven codegen setting.
CROSSFLAGS := -noixemul -m68020 -O2 -fomit-frame-pointer \
              -Wall -Wextra -Wno-unused-parameter

.PHONY: all mcp host-test clean

all: bin/mcp-amiga host-test

# --- Amiga hunk executable ---
bin/mcp-amiga: mcp.c
	mkdir -p bin
	$(DOCKER) m68k-amigaos-gcc $(CROSSFLAGS) -o bin/mcp-amiga mcp.c
	@echo "--- file bin/mcp-amiga ---"
	@file bin/mcp-amiga

mcp: bin/mcp-amiga

# --- host self-test (native) ---
host-test: host_test
	./host_test

host_test: mcp.c
	cc -DHOST_TEST -Wall -Wextra -O2 -o host_test mcp.c

clean:
	rm -f host_test bin/mcp-amiga
