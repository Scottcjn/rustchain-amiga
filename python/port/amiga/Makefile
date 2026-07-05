# MicroPython minimal port for classic m68k AmigaOS (bare-ROM friendly).
# RustChain Amiga Edition, Phase 3 fallback build: unlike the full
# OoZe1911-based build, this binary depends only on ROM libraries
# (exec/dos), so it runs from a bare AROS-ROM boot with no Libs/ on disk.
# Build via ../../../build-min.sh (docker amigadev/crosstools:m68k-amigaos).
include ../../py/mkenv.mk

CROSS_COMPILE = m68k-amigaos-

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

MICROPY_ROM_TEXT_COMPRESSION ?= 1

include $(TOP)/py/py.mk

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)

# -m68020: AROS A4000/040 config baseline, and dodges the proven bebbo
# -m68000 64-bit-shift miscompile (see miner/README.md).
CFLAGS += $(INC) -Wall -std=c99 -noixemul -m68020 -O2 -fomit-frame-pointer $(COPT)
CFLAGS += -Os -DNDEBUG
LDFLAGS += -noixemul -m68020

CSUPEROPT = -Os

SRC_C = \
	main.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/gchelper_generic.c \
	shared/runtime/stdout_helpers.c \

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

OBJ = $(PY_CORE_O) $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

PROG = upython-min

include $(TOP)/py/mkrules.mk
