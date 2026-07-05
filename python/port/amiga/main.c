// MicroPython on classic m68k AmigaOS — port main.
// RustChain Amiga Edition, Phase 3.
//
// Usage from the Amiga shell:
//   upython script.py      run a Python script from a file (primary mode)
//   upython                interactive REPL (stretch goal; the AmigaDOS
//                          console is cooked/line-buffered, so editing is
//                          rough, but it works for simple input)
//
// stdout goes through libnix stdio, so AmigaDOS redirection (>file) works,
// which is how the FS-UAE test harness captures proof on the host side.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"
#include "shared/runtime/gchelper.h"

// libnix: request a bigger stack before main() runs. The ROM shell default
// (4-8 KB) is nowhere near enough for MicroPython's recursive parser.
unsigned long __stack = 256 * 1024;

// m68k REPR_A guard: the GC root pointer section must be 4-byte aligned or
// object pointers decode as qstrs (see the mpstate.h m68k pad). Catch any
// regression at compile time.
#include <stddef.h>
#include "py/mpstate.h"
_Static_assert(offsetof(mp_state_thread_t, dict_locals) % 4 == 0,
    "mp_state_thread_t root pointer section misaligned on m68k");

// 8-byte alignment so GC block/pointer math never sees a 2-mod-4 base.
static char heap[MICROPY_HEAP_SIZE] __attribute__((aligned(8)));

// Execute a chunk of source. Returns 0 on success, 1 on uncaught exception.
static int do_str(const char *src, size_t len, mp_parse_input_kind_t input_kind) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, len, 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t parse_tree = mp_parse(lex, input_kind);
        mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    } else {
        // uncaught exception
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        return 1;
    }
}

static int run_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("upython: cannot open %s\n", path);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        printf("upython: cannot size %s\n", path);
        return 2;
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        printf("upython: out of memory loading %s\n", path);
        return 2;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    int rc = do_str(buf, got, MP_PARSE_FILE_INPUT);
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    int rc = 0;

    mp_stack_ctrl_init();
    // Leave headroom under the 256 KB libnix stack.
    mp_stack_set_limit(192 * 1024);

    gc_init(heap, heap + sizeof(heap));
    mp_init();

    if (argc > 1) {
        rc = run_file(argv[1]);
    } else {
        pyexec_friendly_repl();
    }

    mp_deinit();
    fflush(stdout);
    return rc;
}

// --- HAL ---

void mp_hal_stdout_tx_strn(const char *str, mp_uint_t len) {
    fwrite(str, 1, len, stdout);
    fflush(stdout); // keep host-visible logs current even if the guest hangs
}

int mp_hal_stdin_rx_chr(void) {
    int c = getchar();
    if (c == EOF) {
        // On EOF keep returning ctrl-D so the REPL exits cleanly.
        return 4;
    }
    return c;
}

// --- GC ---

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

// --- stubs required by the core ---

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    // Scripts are loaded by run_file(); imports from files are disabled.
    mp_raise_OSError(MP_ENOENT);
}

mp_import_stat_t mp_import_stat(const char *path) {
    return MP_IMPORT_STAT_NO_EXIST;
}

void nlr_jump_fail(void *val) {
    printf("FATAL: uncaught NLR %p\n", val);
    exit(1);
}

void NORETURN __fatal_error(const char *msg) {
    printf("FATAL: %s\n", msg);
    exit(1);
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("Assertion '%s' failed, at file %s:%d\n", expr, file, line);
    __fatal_error("Assertion failed");
}
#endif
