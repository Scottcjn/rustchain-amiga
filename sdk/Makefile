# librustchain - RustChain SDK for AmigaOS (m68k)
# Cross-compiles via the amigadev/crosstools docker image (bebbo gcc 6.5.0b).
# bebbo/amiga-gcc no longer exists on Docker Hub; this is the maintained build of it.
# Same toolchain pattern as ../miner/Makefile.

DOCKER_IMAGE = amigadev/crosstools:m68k-amigaos
# run as the invoking user so build outputs are not root-owned
DOCKER_USER  = -u $(shell id -u):$(shell id -g)
DOCKER_RUN   = docker run --rm $(DOCKER_USER) -v $(CURDIR):/work -w /work $(DOCKER_IMAGE)
AMIGA_CC     = m68k-amigaos-gcc
AMIGA_AR     = m68k-amigaos-ar
# -m68000 keeps every binary runnable on any Amiga; the CPU type is
# detected at runtime from ExecBase AttnFlags, not baked in at compile time.
AMIGA_CFLAGS = -noixemul -m68000 -O2 -fomit-frame-pointer -Wall -Wextra -Iinclude

HOST_CC      = gcc
HOST_CFLAGS  = -DHOST_TEST -O2 -Wall -Wextra -Iinclude

SRCS = src/rc_u64.c src/rc_sha1.c src/rc_json.c src/rc_hw.c src/rc_http.c src/rc_attest.c
LIB  = lib/librustchain.a

all: lib examples

# static library: compile every module and archive it, one docker run
lib: $(LIB)

$(LIB): $(SRCS) include/rustchain/*.h
	$(DOCKER_RUN) sh -c 'set -e; mkdir -p build lib; \
	  for f in $(SRCS); do \
	    echo "CC  $$f"; \
	    $(AMIGA_CC) $(AMIGA_CFLAGS) -c $$f -o build/$$(basename $$f .c).o; \
	  done; \
	  rm -f $(LIB); \
	  $(AMIGA_AR) rcs $(LIB) build/*.o'
	@ls -l $(LIB)

# examples link against the freshly built archive
examples: $(LIB) examples/attest_demo.c examples/hello_rustchain.c
	$(DOCKER_RUN) sh -c 'set -e; \
	  $(AMIGA_CC) $(AMIGA_CFLAGS) -o examples/attest_demo examples/attest_demo.c $(LIB); \
	  $(AMIGA_CC) $(AMIGA_CFLAGS) -o examples/hello_rustchain examples/hello_rustchain.c $(LIB)'
	file examples/attest_demo examples/hello_rustchain

# native host test of the portable logic (SHA-1, u64 math, JSON, HTTP
# formatting) plus a python json.loads pass over the built payload
host-test: host_test host-test32
	./host_test
	./host_test --payload | python3 -c "import json,sys; json.loads(sys.stdin.read()); print('python json.loads: OK')"

host_test: tests/host_test.c $(SRCS) include/rustchain/*.h
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/host_test.c $(SRCS)

# 32-bit ILP32 run (long = 4 bytes, same as m68k). Catches int-promotion
# and libgcc 64-bit-op bugs the x86_64 test cannot see. Plain gcc -m32
# lacks multilib libs on this box, the i386 docker image is the
# no-system-changes way to get a true 32-bit userland.
HOST32_IMAGE = i386/gcc
host-test32: tests/host_test.c $(SRCS)
	docker run --rm $(DOCKER_USER) -v $(CURDIR):/work -w /work $(HOST32_IMAGE) \
	  sh -c "gcc $(HOST_CFLAGS) -o host_test32 tests/host_test.c $(SRCS) && ./host_test32"

clean:
	rm -rf build $(LIB) examples/attest_demo examples/hello_rustchain host_test host_test32

.PHONY: all lib examples host-test host-test32 clean
