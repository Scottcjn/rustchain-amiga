// MicroPython port to classic m68k AmigaOS (RustChain Amiga Edition, Phase 3)
// Cross-built with bebbo gcc 6.5.0b (amigadev/crosstools:m68k-amigaos), -noixemul/libnix.
// Target baseline: 68020+ (AROS m68k config emulates a 68040 A4000).
//
// Design notes:
// - MICROPY_NLR_SETJMP: upstream has no m68k asm NLR, libnix setjmp works.
// - MICROPY_GCREGS_SETJMP: register roots for the GC come from a setjmp buffer
//   (shared/runtime/gchelper_generic.c), no m68k-specific helper needed.
// - Long ints via mpz (16-bit digits on this ILP32 target) so 2**40 math works
//   WITHOUT 64-bit shifts, dodging the proven bebbo -m68000 64-bit-shift
//   miscompile (see miner/README.md). We build -m68020 anyway.
// - No float to start (spec: none or single). Keeps libnix math out of it.

#include <stdint.h>

// Rich-but-small feature set. CORE gives dicts, str methods, sys, etc.
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

#define MICROPY_ENABLE_COMPILER     (1)
#define MICROPY_ENABLE_GC           (1)
#define MICROPY_HELPER_REPL         (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)

#define MICROPY_LONGINT_IMPL        (MICROPY_LONGINT_IMPL_MPZ)

// No filesystem-backed io module in the minimal build (scripts are loaded
// by main.c directly); avoids needing mp_builtin_open.
#define MICROPY_PY_IO               (0)
#define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_NONE)

// Non-local return via setjmp/longjmp (no m68k asm NLR upstream).
#define MICROPY_NLR_SETJMP          (1)

// CRITICAL m68k fix: the amigadev/crosstools bebbo gcc aligns global const
// structs to 2 bytes (proven via link map: mp_builtin_print_obj at a 2-mod-4
// address). Under MICROPY_OBJ_REPR_A a 2-mod-4 object pointer decodes as a
// qstr, so every builtin "becomes a str". Force 4-byte alignment on every
// struct that embeds mp_obj_base_t.
#define MICROPY_OBJ_BASE_ALIGNMENT  __attribute__((aligned(4)))

// Register scanning for GC via setjmp (gchelper_generic.c).
#define MICROPY_GCREGS_SETJMP       (1)

// AmigaOS shell stacks are small; we self-extend via libnix __stack but keep
// the checker on so deep recursion raises RuntimeError instead of trashing RAM.
#define MICROPY_STACK_CHECK         (1)

#define MICROPY_ENABLE_SOURCE_LINE  (1)
#define MICROPY_ERROR_REPORTING     (MICROPY_ERROR_REPORTING_NORMAL)

#define MICROPY_ALLOC_PATH_MAX      (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT (16)

// Static heap, per spec: 512 KB (lives in BSS; hunk BSS costs no file size).
#define MICROPY_HEAP_SIZE           (512 * 1024)

// type definitions for the specific machine (m68k is ILP32)
typedef intptr_t mp_int_t; // must be pointer size
typedef uintptr_t mp_uint_t; // must be pointer size
typedef long mp_off_t;

// libnix has no <alloca.h>; gcc builtin does the job.
#ifndef alloca
#define alloca(n) __builtin_alloca(n)
#endif

#define MICROPY_HW_BOARD_NAME "Amiga (AROS/FS-UAE)"
#define MICROPY_HW_MCU_NAME   "m68k"

#define MICROPY_PY_SYS_PLATFORM "amiga"

#define MP_STATE_PORT MP_STATE_VM
