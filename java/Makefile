# RustChain Amiga Edition - Phase 3 java/ workstream
#
# Targets:
#   make hello       - compile HelloWorld.java to Java 8 bytecode (host javac)
#   make host        - build mjvm natively and run HelloWorld.class on the host
#   make amiga       - cross-compile mjvm to an AmigaOS hunk executable (docker)
#   make stage       - copy mjvm + HelloWorld.class into test/shared/ for FS-UAE
#   make test-emu    - boot FS-UAE headless, run the class inside the Amiga,
#                      verify the output from the host
#   make all         - hello + host + amiga + stage
#
# Toolchain: amigadev/crosstools:m68k-amigaos (bebbo gcc 6.5.0b), same image
# the miner uses. -m68020 per project guidance (bebbo gcc miscompiles 64-bit
# shifts at -m68000; mjvm has no 64-bit ops by design, but stay on the safe
# setting; the test machine is an emulated 68040).

DOCKER_IMG = amigadev/crosstools:m68k-amigaos
CROSS_CC   = m68k-amigaos-gcc
CROSS_FLAGS = -noixemul -m68020 -O2 -fomit-frame-pointer -Wall -Wextra
HOST_CC    = cc
HOST_FLAGS = -std=c89 -O2 -Wall -Wextra -Wno-long-long

all: hello host amiga stage

hello: hello/HelloWorld.class

hello/HelloWorld.class: hello/HelloWorld.java
	javac --release 8 -d hello hello/HelloWorld.java

host: vm/mjvm_host hello/HelloWorld.class
	./vm/mjvm_host hello/HelloWorld.class

vm/mjvm_host: vm/mjvm.c
	$(HOST_CC) $(HOST_FLAGS) -o vm/mjvm_host vm/mjvm.c

amiga: vm/mjvm

vm/mjvm: vm/mjvm.c
	docker run --rm -v $(CURDIR)/vm:/work -w /work $(DOCKER_IMG) \
	    $(CROSS_CC) $(CROSS_FLAGS) -o mjvm mjvm.c
	file vm/mjvm

stage: vm/mjvm hello/HelloWorld.class
	mkdir -p test/shared/S
	cp vm/mjvm test/shared/mjvm
	cp hello/HelloWorld.class test/shared/HelloWorld.class

test-emu: stage
	rm -f test/shared/java.log test/shared/java.log.uaem test/shared/boot_proof.txt
	timeout 240 xvfb-run --auto-servernum -s "-screen 0 1280x1024x24" \
	    fs-uae test/java-test.fs-uae > test/fsuae_run.log 2>&1 || true
	@echo "--- guest java.log ---"
	@cat test/shared/java.log

clean:
	rm -f vm/mjvm vm/mjvm_host hello/HelloWorld.class \
	      test/shared/mjvm test/shared/HelloWorld.class

.PHONY: all hello host amiga stage test-emu clean
