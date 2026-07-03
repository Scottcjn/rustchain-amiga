/*
 * mjvm.c - micro JVM: a minimal Java class-file interpreter
 *
 * RustChain Amiga Edition, Phase 3 java/ workstream.
 *
 * Purpose: run real javac-produced Java bytecode (class file format 52 /
 * Java 8 or older) on classic m68k AmigaOS, where no JVM exists and GCJ
 * cannot be built (see java/FEASIBILITY.md). This is deliberately a SUBSET
 * interpreter, not a full JVM. What is in and what is out is documented
 * below and in java/README.md. Honest subset, no pretending.
 *
 * Requirements: ANSI C (C89), stdio, malloc, exit. No threads, no mmap,
 * no OS calls. Compiles with bebbo m68k-amigaos-gcc 6.5.0b (-noixemul,
 * libnix) and any host cc for testing.
 *
 * Design constraints for m68k-amigaos:
 *  - ILP32: int and pointers are 32-bit. All JVM slots are "unsigned int".
 *  - NO 64-bit arithmetic anywhere. bebbo gcc 6.5 is known to miscompile
 *    64-bit shifts at -m68000 (see miner/README.md); this VM designs the
 *    problem out by not supporting Java long/float/double at all.
 *  - No %lld, no %ld needed: everything printed is a 32-bit int.
 *  - Class files are big-endian; readers are explicit byte-compose shifts,
 *    so the same code is correct on big-endian m68k and little-endian x86.
 *
 * Supported subset:
 *  - Class file: single class, format major <= 52 (javac --release 8).
 *    Full constant-pool parse (all tags up to 20, long/double entries are
 *    parsed and slot-skipped correctly but cannot be used).
 *  - Types: int (and boolean/byte/char/short as ints), java.lang.String
 *    literals, int[] arrays. NO long, float, double, objects, or classes
 *    other than the loaded one.
 *  - Opcodes: all int constants/loads/stores/arithmetic/logic/comparisons,
 *    iinc, all int branches, goto/goto_w, tableswitch, lookupswitch,
 *    dup/dup_x1/dup_x2/dup2/pop/pop2/swap, i2b/i2c/i2s, newarray(int),
 *    iaload/iastore/arraylength, ldc/ldc_w (int + String), wide,
 *    invokestatic (static methods of the SAME class only),
 *    getstatic (java/lang/System.out and .err only),
 *    invokevirtual (PrintStream print/println for int, char, boolean,
 *    String, and the no-arg println), ireturn/areturn/return,
 *    ifnull/ifnonnull/if_acmpeq/if_acmpne, aconst_null.
 *  - Java semantics honored: 32-bit wrapping arithmetic, shift counts
 *    masked to 5 bits, idiv/irem truncate toward zero, INT_MIN/-1 = INT_MIN,
 *    division by zero is a fatal VM error (no exception objects).
 *  - NOT supported (fatal error with message): long/float/double bytecodes,
 *    new/instance objects, string concatenation (StringBuilder), fields,
 *    other classes, exceptions, threads, monitorenter/exit, invokedynamic.
 *
 * Exit codes: 0 = program ran to completion, 20 = VM error (Amiga "FAIL").
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MJVM_VERSION "0.1"

typedef unsigned char  u1;
typedef unsigned short u2;
typedef unsigned int   u4;   /* 32-bit on m68k-amigaos AND x86_64 hosts */
typedef signed   int   s4;
typedef unsigned int   slot; /* one JVM operand-stack / local-var slot   */

/* compile-time guarantee that the types are the sizes we assume */
typedef char mjvm_assert_u2[(sizeof(u2) == 2) ? 1 : -1];
typedef char mjvm_assert_u4[(sizeof(u4) == 4) ? 1 : -1];

/* ---- reference encoding in 32-bit slots -------------------------------
 * Verified bytecode never confuses ints with references, and this VM only
 * decodes a slot as a reference where the opcode/descriptor says it is one.
 * REF_NULL   0
 * REF_STDOUT / REF_STDERR   magic singletons for System.out / System.err
 * string ref 0x60000000 | utf8-constant-pool-index
 * array  ref 0x50000000 | (array-table-index + 1)
 */
#define REF_NULL    0u
#define REF_STDOUT  0x7f000001u
#define REF_STDERR  0x7f000002u
#define REF_STR_TAG 0x60000000u
#define REF_ARR_TAG 0x50000000u
#define REF_TAGMASK 0xf0000000u
#define REF_IDXMASK 0x0fffffffu

/* ---- fatal error ------------------------------------------------------ */
static void die(const char *msg)
{
    printf("mjvm: FATAL: %s\n", msg);
    exit(20);
}
static void die2(const char *msg, const char *detail)
{
    printf("mjvm: FATAL: %s: %s\n", msg, detail);
    exit(20);
}
static void die_op(const char *msg, int op, int pc)
{
    printf("mjvm: FATAL: %s (opcode %d at pc %d)\n", msg, op, pc);
    exit(20);
}

/* ---- big-endian readers (portable on BE and LE hosts) ------------------ */
static u2 rd_u2(const u1 *p) { return (u2)(((u2)p[0] << 8) | p[1]); }
static u4 rd_u4(const u1 *p)
{
    return ((u4)p[0] << 24) | ((u4)p[1] << 16) | ((u4)p[2] << 8) | (u4)p[3];
}
static int rd_s2(const u1 *p) { return (int)(short)rd_u2(p); }
static s4  rd_s4(const u1 *p) { return (s4)rd_u4(p); }

/* ---- constant pool ----------------------------------------------------- */
#define CP_UTF8            1
#define CP_INTEGER         3
#define CP_FLOAT           4
#define CP_LONG            5
#define CP_DOUBLE          6
#define CP_CLASS           7
#define CP_STRING          8
#define CP_FIELDREF        9
#define CP_METHODREF      10
#define CP_IFACEMETHODREF 11
#define CP_NAMEANDTYPE    12
#define CP_METHODHANDLE   15
#define CP_METHODTYPE     16
#define CP_DYNAMIC        17
#define CP_INVOKEDYNAMIC  18
#define CP_MODULE         19
#define CP_PACKAGE        20

typedef struct {
    u1 tag;
    u2 a, b;            /* generic index operands */
    s4 ival;            /* CP_INTEGER value */
    const u1 *utf8;     /* CP_UTF8 bytes (not NUL terminated) */
    u2 utf8_len;
} cp_t;

typedef struct {
    u2 flags, name_idx, desc_idx;
    const u1 *code;
    u4 code_len;
    u2 max_stack, max_locals;
} method_t;

typedef struct {
    u1 *buf;            /* whole class file in memory */
    long buf_len;
    u2 major;
    u2 cp_count;
    cp_t *cp;
    u2 this_class;
    u2 method_count;
    method_t *methods;
} class_t;

/* ---- int[] array table -------------------------------------------------- */
#define MAX_ARRAYS 512
static s4 *g_arr[MAX_ARRAYS];
static s4  g_arr_len[MAX_ARRAYS];
static int g_arr_count = 0;

static slot array_new(s4 count)
{
    s4 *p;
    if (count < 0) die("NegativeArraySizeException");
    if (g_arr_count >= MAX_ARRAYS) die("too many arrays (VM limit)");
    p = (s4 *)calloc((size_t)(count ? count : 1), sizeof(s4));
    if (!p) die("out of memory allocating array");
    g_arr[g_arr_count] = p;
    g_arr_len[g_arr_count] = count;
    g_arr_count++;
    return REF_ARR_TAG | (u4)g_arr_count; /* index+1, 0 stays null */
}
static int array_deref(slot ref)
{
    u4 idx;
    if (ref == REF_NULL) die("NullPointerException (array is null)");
    if ((ref & REF_TAGMASK) != REF_ARR_TAG) die("VM error: not an array reference");
    idx = (ref & REF_IDXMASK);
    if (idx == 0 || (int)idx > g_arr_count) die("VM error: bad array reference");
    return (int)idx - 1;
}

/* ---- cp helpers --------------------------------------------------------- */
static cp_t *cp_get(class_t *C, u2 idx, u1 want_tag, const char *what)
{
    if (idx == 0 || idx >= C->cp_count) die2("bad constant pool index", what);
    if (want_tag && C->cp[idx].tag != want_tag) die2("wrong constant pool tag", what);
    return &C->cp[idx];
}
static int utf8_eq(class_t *C, u2 utf8_idx, const char *s)
{
    cp_t *e = cp_get(C, utf8_idx, CP_UTF8, "utf8_eq");
    size_t n = strlen(s);
    return e->utf8_len == n && memcmp(e->utf8, s, n) == 0;
}
static void utf8_print(class_t *C, u2 utf8_idx)
{
    cp_t *e = cp_get(C, utf8_idx, CP_UTF8, "utf8_print");
    fwrite(e->utf8, 1, e->utf8_len, stdout);
}

/* ---- class file parsing -------------------------------------------------- */
/* bounds notes: p is always kept <= end, so (u4)(end - p) is the number of
   bytes still available. Comparing that count against a length avoids the
   pointer-overflow trap of "p + len > end" on ILP32 m68k, where a huge len
   can wrap the pointer back below end and slip past the check. */
static const u1 *skip_attributes(class_t *C, const u1 *p, const u1 *end)
{
    u2 count, i;
    (void)C;
    if (end - p < 2) die("truncated class file (attribute count)");
    count = rd_u2(p); p += 2;
    for (i = 0; i < count; i++) {
        u4 len;
        if (end - p < 6) die("truncated class file (attribute header)");
        len = rd_u4(p + 2);
        p += 6;
        if (len > (u4)(end - p)) die("truncated class file (attribute body)");
        p += len;
    }
    return p;
}

static void parse_code_attr(class_t *C, method_t *m, const u1 *p, const u1 *end)
{
    u2 exc_count;
    (void)C;
    if (end - p < 8) die("truncated Code attribute (header)");
    m->max_stack  = rd_u2(p);
    m->max_locals = rd_u2(p + 2);
    m->code_len   = rd_u4(p + 4);
    m->code       = p + 8;
    if (m->code_len > (u4)(end - (p + 8)))
        die("truncated Code attribute (code longer than attribute)");
    p = m->code + m->code_len;
    if (end - p < 2) die("truncated Code attribute (exception count)");
    exc_count = rd_u2(p); p += 2;
    if ((u4)exc_count * 8 > (u4)(end - p))
        die("truncated Code attribute (exception table)");
    p += (u4)exc_count * 8;      /* exception table ignored (no exceptions) */
    /* code attribute's own attributes (StackMapTable etc.) ignored */
}

static void class_load(class_t *C, const char *path)
{
    FILE *f;
    const u1 *p, *end;
    u2 i, count;

    memset(C, 0, sizeof(*C));
    f = fopen(path, "rb");
    if (!f) die2("cannot open class file", path);
    fseek(f, 0, SEEK_END);
    C->buf_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (C->buf_len <= 0 || C->buf_len > 4L * 1024 * 1024)
        die("class file empty or unreasonably large");
    C->buf = (u1 *)malloc((size_t)C->buf_len);
    if (!C->buf) die("out of memory loading class file");
    if (fread(C->buf, 1, (size_t)C->buf_len, f) != (size_t)C->buf_len)
        die("short read on class file");
    fclose(f);

    p = C->buf;
    end = C->buf + C->buf_len;
    if (C->buf_len < 24 || rd_u4(p) != 0xCAFEBABEu)
        die("not a Java class file (bad magic)");
    C->major = rd_u2(p + 6);
    if (C->major > 52)
        die("class file newer than Java 8; recompile with: javac --release 8");
    p += 8;

    /* constant pool */
    C->cp_count = rd_u2(p); p += 2;
    C->cp = (cp_t *)calloc(C->cp_count ? C->cp_count : 1, sizeof(cp_t));
    if (!C->cp) die("out of memory (constant pool)");
    for (i = 1; i < C->cp_count; i++) {
        cp_t *e = &C->cp[i];
        if (p >= end) die("truncated constant pool");
        e->tag = *p++;
        switch (e->tag) {
        case CP_UTF8:
            if (end - p < 2) die("truncated constant pool (utf8 length)");
            e->utf8_len = rd_u2(p);
            if ((u4)e->utf8_len > (u4)(end - (p + 2)))
                die("truncated constant pool (utf8 body)");
            e->utf8 = p + 2;
            p += 2 + e->utf8_len;
            break;
        case CP_INTEGER:
            if (end - p < 4) die("truncated constant pool (integer)");
            e->ival = rd_s4(p); p += 4; break;
        case CP_FLOAT:
            if (end - p < 4) die("truncated constant pool (float)");
            p += 4; break;                     /* parsed, unusable */
        case CP_LONG:
        case CP_DOUBLE:
            if (end - p < 8) die("truncated constant pool (long/double)");
            p += 8; i++; break;                /* takes two cp slots */
        case CP_CLASS: case CP_STRING: case CP_METHODTYPE:
        case CP_MODULE: case CP_PACKAGE:
            if (end - p < 2) die("truncated constant pool (index entry)");
            e->a = rd_u2(p); p += 2; break;
        case CP_FIELDREF: case CP_METHODREF: case CP_IFACEMETHODREF:
        case CP_NAMEANDTYPE: case CP_DYNAMIC: case CP_INVOKEDYNAMIC:
            if (end - p < 4) die("truncated constant pool (ref entry)");
            e->a = rd_u2(p); e->b = rd_u2(p + 2); p += 4; break;
        case CP_METHODHANDLE:
            if (end - p < 3) die("truncated constant pool (methodhandle)");
            e->a = *p; e->b = rd_u2(p + 1); p += 3; break;
        default:
            die("unknown constant pool tag");
        }
        if (p > end) die("truncated constant pool entry");
    }

    /* access flags, this/super class */
    if (end - p < 6) die("truncated class file (this/super class)");
    C->this_class = rd_u2(p + 2);
    p += 6;

    /* interfaces */
    if (end - p < 2) die("truncated class file (interface count)");
    count = rd_u2(p); p += 2;
    if ((u4)count * 2 > (u4)(end - p))
        die("truncated class file (interface table)");
    p += (u4)count * 2;

    /* fields: skipped entirely (no field support) */
    if (end - p < 2) die("truncated class file (field count)");
    count = rd_u2(p); p += 2;
    for (i = 0; i < count; i++) {
        if (end - p < 6) die("truncated class file (field entry)");
        p += 6;
        p = skip_attributes(C, p, end);
    }

    /* methods */
    if (end - p < 2) die("truncated class file (method count)");
    C->method_count = rd_u2(p); p += 2;
    C->methods = (method_t *)calloc(C->method_count ? C->method_count : 1,
                                    sizeof(method_t));
    if (!C->methods) die("out of memory (methods)");
    for (i = 0; i < C->method_count; i++) {
        method_t *m = &C->methods[i];
        u2 acount, a;
        if (end - p < 6) die("truncated class file (method entry)");
        m->flags    = rd_u2(p);
        m->name_idx = rd_u2(p + 2);
        m->desc_idx = rd_u2(p + 4);
        p += 6;
        if (end - p < 2) die("truncated class file (method attr count)");
        acount = rd_u2(p); p += 2;
        for (a = 0; a < acount; a++) {
            u2 aname;
            u4 alen;
            if (end - p < 6) die("truncated class file (method attr header)");
            aname = rd_u2(p);
            alen  = rd_u4(p + 2);
            p += 6;
            if (alen > (u4)(end - p)) die("truncated method attribute");
            if (utf8_eq(C, aname, "Code"))
                parse_code_attr(C, m, p, p + alen);
            p += alen;
        }
    }
    /* class attributes: not needed */
}

static method_t *find_method(class_t *C, const char *name, const char *desc)
{
    u2 i;
    for (i = 0; i < C->method_count; i++) {
        method_t *m = &C->methods[i];
        if (utf8_eq(C, m->name_idx, name) && utf8_eq(C, m->desc_idx, desc))
            return m;
    }
    return NULL;
}

/* descriptor: count argument slots; only int-like and reference args allowed.
 * returns arg slot count; *ret_kind: 0 = void, 1 = int-like, 2 = reference */
static int desc_args(class_t *C, u2 desc_idx, int *ret_kind)
{
    cp_t *e = cp_get(C, desc_idx, CP_UTF8, "method descriptor");
    const u1 *p = e->utf8, *end = e->utf8 + e->utf8_len;
    int n = 0;
    if (p >= end || *p != '(') die("bad method descriptor");
    p++;
    while (p < end && *p != ')') {
        switch (*p) {
        case 'I': case 'Z': case 'B': case 'C': case 'S':
            n++; p++; break;
        case 'L':
            while (p < end && *p != ';') p++;
            if (p >= end) die("bad method descriptor (unterminated L)");
            n++; p++; break;
        case '[':
            while (p < end && *p == '[') p++;
            if (p < end && *p == 'L') {
                while (p < end && *p != ';') p++;
                if (p >= end) die("bad descriptor (unterminated [L)");
            }
            n++; p++; break;
        case 'J': case 'D': case 'F':
            die("unsupported argument type in descriptor (long/float/double)");
            break;
        default:
            die("bad method descriptor char");
        }
    }
    if (p >= end) die("bad method descriptor (no close paren)");
    p++;
    if (p >= end) die("bad method descriptor (no return type)");
    switch (*p) {
    case 'V': *ret_kind = 0; break;
    case 'I': case 'Z': case 'B': case 'C': case 'S': *ret_kind = 1; break;
    case 'L': case '[': *ret_kind = 2; break;
    default: die("unsupported return type (long/float/double)"); *ret_kind = 0;
    }
    return n;
}

/* ---- PrintStream natives ------------------------------------------------ */
static FILE *stream_of(slot ref)
{
    if (ref == REF_STDOUT) return stdout;
    if (ref == REF_STDERR) return stdout; /* single console on the Amiga log */
    if (ref == REF_NULL) die("NullPointerException (print on null stream)");
    die("VM error: invokevirtual receiver is not System.out/err");
    return NULL;
}
static void native_print_str(class_t *C, FILE *f, slot v, int nl)
{
    if (v == REF_NULL) {
        fputs("null", f);
    } else if ((v & REF_TAGMASK) == REF_STR_TAG) {
        cp_t *e = cp_get(C, (u2)(v & REF_IDXMASK), CP_UTF8, "string ref");
        fwrite(e->utf8, 1, e->utf8_len, f);
    } else {
        die("VM error: println(String) argument is not a string literal");
    }
    if (nl) fputc('\n', f);
}

/* ---- interpreter --------------------------------------------------------- */
static void run(class_t *C, method_t *m, slot *args, int nargs,
                slot *retval, int *has_ret);

/* invoke helper shared by invokestatic */
static void invoke_static(class_t *C, u2 mref_idx, slot *stack, int *sp)
{
    cp_t *mref, *cls, *nat;
    method_t *callee;
    char name[128], desc[128];
    cp_t *nu, *du;
    int argc, ret_kind, has_ret, i;
    slot argbuf[32], rv;

    mref = cp_get(C, mref_idx, CP_METHODREF, "invokestatic");
    cls  = cp_get(C, mref->a, CP_CLASS, "invokestatic class");
    nat  = cp_get(C, mref->b, CP_NAMEANDTYPE, "invokestatic nat");

    /* only static calls into THIS class are supported */
    {
        cp_t *this_cls = cp_get(C, C->this_class, CP_CLASS, "this_class");
        cp_t *cn  = cp_get(C, cls->a, CP_UTF8, "callee class name");
        cp_t *tn  = cp_get(C, this_cls->a, CP_UTF8, "this class name");
        if (cn->utf8_len != tn->utf8_len ||
            memcmp(cn->utf8, tn->utf8, cn->utf8_len) != 0)
            die("invokestatic to another class is not supported "
                "(single-class VM)");
    }

    nu = cp_get(C, nat->a, CP_UTF8, "method name");
    du = cp_get(C, nat->b, CP_UTF8, "method descriptor");
    if (nu->utf8_len >= sizeof(name) || du->utf8_len >= sizeof(desc))
        die("method name/descriptor too long");
    memcpy(name, nu->utf8, nu->utf8_len); name[nu->utf8_len] = 0;
    memcpy(desc, du->utf8, du->utf8_len); desc[du->utf8_len] = 0;

    callee = find_method(C, name, desc);
    if (!callee) die2("static method not found in class", name);
    if (!callee->code) die2("method has no Code (abstract/native?)", name);

    argc = desc_args(C, nat->b, &ret_kind);
    if (argc > 32) die("too many arguments (VM limit 32)");
    if (*sp < argc) die("operand stack underflow on invokestatic");
    for (i = argc - 1; i >= 0; i--) argbuf[i] = stack[--(*sp)];

    has_ret = 0; rv = 0;
    run(C, callee, argbuf, argc, &rv, &has_ret);
    if (ret_kind != 0) {
        if (!has_ret) die("method did not return a value");
        stack[(*sp)++] = rv;
    }
}

/* the instruction starting at pc needs nbytes total (opcode + operands);
   every byte in [pc, pc + nbytes) must be inside the method's code array.
   Written to be safe against unsigned wrap: check the length first, then
   the position. Any out-of-range operand is a controlled fatal error, not
   a read past the end of the code buffer. */
static void code_need(const method_t *m, u4 pc, u4 nbytes)
{
    if (nbytes > m->code_len || pc > m->code_len - nbytes)
        die("truncated bytecode (operand runs past end of code)");
}

/* Bounds-checked local-variable access. A corrupt or hostile class file can
   carry a local index (in iload/istore/iinc/wide) larger than max_locals;
   without this the interpreter would read or write past the locals frame.
   Returns an lvalue pointer so the same guard serves loads and stores. */
static slot *local_ref(slot *locals, const method_t *m, u4 idx)
{
    if (idx >= (u4)m->max_locals)
        die("local variable index out of range");
    return &locals[idx];
}

static void run(class_t *C, method_t *m, slot *args, int nargs,
                slot *retval, int *has_ret)
{
    slot *locals, *stack;
    int sp = 0;
    u4 pc = 0;
    const u1 *code = m->code;
    static int depth = 0;

    if (!code) die("attempt to run method without Code");
    if (++depth > 512) die("StackOverflowError (VM recursion limit 512)");

    locals = (slot *)calloc(m->max_locals ? m->max_locals : 1, sizeof(slot));
    stack  = (slot *)calloc((size_t)m->max_stack + 4, sizeof(slot));
    if (!locals || !stack) die("out of memory (frame)");
    if (nargs > m->max_locals) die("more args than locals");
    if (nargs > 0) memcpy(locals, args, (size_t)nargs * sizeof(slot));

#define PUSH(v)  do { if (sp >= (int)m->max_stack) die("operand stack overflow"); \
                      stack[sp++] = (slot)(v); } while (0)
#define POP()    (sp > 0 ? stack[--sp] : (die("operand stack underflow"), 0u))
#define IPOP()   ((s4)POP())
#define LGET(i)  (*local_ref(locals, m, (u4)(i)))
#define BRANCH16() do { code_need(m, pc, 3); \
                        pc = (u4)((s4)pc + rd_s2(code + pc + 1)); } while (0)

    for (;;) {
        u1 op;
        if (pc >= m->code_len) die("pc ran off end of code");
        op = code[pc];
        switch (op) {
        case 0: pc++; break;                                    /* nop */
        case 1: PUSH(REF_NULL); pc++; break;                    /* aconst_null */
        case 2: case 3: case 4: case 5: case 6: case 7: case 8: /* iconst_m1..5 */
            PUSH((s4)op - 3); pc++; break;
        case 16:                                                /* bipush */
            code_need(m, pc, 2);
            PUSH((s4)(signed char)code[pc + 1]); pc += 2; break;
        case 17:                                                /* sipush */
            code_need(m, pc, 3);
            PUSH(rd_s2(code + pc + 1)); pc += 3; break;
        case 18: case 19: {                                     /* ldc, ldc_w */
            u2 idx;
            cp_t *e;
            code_need(m, pc, (op == 18) ? 2 : 3);
            idx = (op == 18) ? code[pc + 1] : rd_u2(code + pc + 1);
            e = cp_get(C, idx, 0, "ldc");
            if (e->tag == CP_INTEGER) PUSH(e->ival);
            else if (e->tag == CP_STRING) PUSH(REF_STR_TAG | e->a);
            else die("ldc of unsupported constant (float/long/double/class?)");
            pc += (op == 18) ? 2 : 3;
            break; }
        case 21: case 25:                                       /* iload, aload */
            code_need(m, pc, 2);
            PUSH(LGET(code[pc + 1])); pc += 2; break;
        case 26: case 27: case 28: case 29:                     /* iload_0..3 */
            PUSH(LGET(op - 26)); pc++; break;
        case 42: case 43: case 44: case 45:                     /* aload_0..3 */
            PUSH(LGET(op - 42)); pc++; break;
        case 46: {                                              /* iaload */
            s4 idx = IPOP(); int a = array_deref(POP());
            if (idx < 0 || idx >= g_arr_len[a])
                die("ArrayIndexOutOfBoundsException");
            PUSH(g_arr[a][idx]); pc++; break; }
        case 54: case 58:                                       /* istore, astore */
            code_need(m, pc, 2);
            LGET(code[pc + 1]) = POP(); pc += 2; break;
        case 59: case 60: case 61: case 62:                     /* istore_0..3 */
            LGET(op - 59) = POP(); pc++; break;
        case 75: case 76: case 77: case 78:                     /* astore_0..3 */
            LGET(op - 75) = POP(); pc++; break;
        case 79: {                                              /* iastore */
            s4 val = IPOP(); s4 idx = IPOP(); int a = array_deref(POP());
            if (idx < 0 || idx >= g_arr_len[a])
                die("ArrayIndexOutOfBoundsException");
            g_arr[a][idx] = val; pc++; break; }
        case 87: (void)POP(); pc++; break;                      /* pop */
        case 88: (void)POP(); (void)POP(); pc++; break;         /* pop2 */
        case 89: {                                              /* dup */
            slot v = POP(); PUSH(v); PUSH(v); pc++; break; }
        case 90: {                                              /* dup_x1 */
            slot v1 = POP(), v2 = POP();
            PUSH(v1); PUSH(v2); PUSH(v1); pc++; break; }
        case 91: {                                              /* dup_x2 */
            slot v1 = POP(), v2 = POP(), v3 = POP();
            PUSH(v1); PUSH(v3); PUSH(v2); PUSH(v1); pc++; break; }
        case 92: {                                              /* dup2 */
            slot v1 = POP(), v2 = POP();
            PUSH(v2); PUSH(v1); PUSH(v2); PUSH(v1); pc++; break; }
        case 95: {                                              /* swap */
            slot v1 = POP(), v2 = POP(); PUSH(v1); PUSH(v2); pc++; break; }
        case 96: { s4 b = IPOP(), a = IPOP();                   /* iadd */
            PUSH((s4)((u4)a + (u4)b)); pc++; break; }
        case 100: { s4 b = IPOP(), a = IPOP();                  /* isub */
            PUSH((s4)((u4)a - (u4)b)); pc++; break; }
        case 104: { s4 b = IPOP(), a = IPOP();                  /* imul */
            PUSH((s4)((u4)a * (u4)b)); pc++; break; }
        case 108: { s4 b = IPOP(), a = IPOP();                  /* idiv */
            if (b == 0) die("ArithmeticException: / by zero");
            if (a == (s4)0x80000000 && b == -1) PUSH(a);
            else PUSH(a / b);
            pc++; break; }
        case 112: { s4 b = IPOP(), a = IPOP();                  /* irem */
            if (b == 0) die("ArithmeticException: % by zero");
            if (a == (s4)0x80000000 && b == -1) PUSH(0);
            else PUSH(a % b);
            pc++; break; }
        case 116: { s4 a = IPOP();                              /* ineg */
            PUSH((s4)(0u - (u4)a)); pc++; break; }
        case 120: { s4 b = IPOP(), a = IPOP();                  /* ishl */
            PUSH((s4)((u4)a << (b & 31))); pc++; break; }
        case 122: { s4 b = IPOP(), a = IPOP();                  /* ishr */
            /* arithmetic shift; C impl-defined on negatives, do it manually */
            int sh = b & 31;
            if (a >= 0) PUSH(a >> sh);
            else PUSH((s4)(~((~(u4)a) >> sh)));
            pc++; break; }
        case 124: { s4 b = IPOP(); u4 a = (u4)IPOP();           /* iushr */
            PUSH((s4)(a >> (b & 31))); pc++; break; }
        case 126: { s4 b = IPOP(), a = IPOP(); PUSH(a & b); pc++; break; } /* iand */
        case 128: { s4 b = IPOP(), a = IPOP(); PUSH(a | b); pc++; break; } /* ior  */
        case 130: { s4 b = IPOP(), a = IPOP(); PUSH(a ^ b); pc++; break; } /* ixor */
        case 132:                                               /* iinc */
            code_need(m, pc, 3);
            LGET(code[pc + 1]) =
                (slot)((s4)LGET(code[pc + 1]) + (signed char)code[pc + 2]);
            pc += 3; break;
        case 145: { s4 a = IPOP();                              /* i2b */
            PUSH((s4)(signed char)(a & 0xff)); pc++; break; }
        case 146: { s4 a = IPOP();                              /* i2c */
            PUSH(a & 0xffff); pc++; break; }
        case 147: { s4 a = IPOP();                              /* i2s */
            PUSH((s4)(short)(a & 0xffff)); pc++; break; }
        case 153: { if (IPOP() == 0) BRANCH16(); else pc += 3; break; } /* ifeq */
        case 154: { if (IPOP() != 0) BRANCH16(); else pc += 3; break; } /* ifne */
        case 155: { if (IPOP() <  0) BRANCH16(); else pc += 3; break; } /* iflt */
        case 156: { if (IPOP() >= 0) BRANCH16(); else pc += 3; break; } /* ifge */
        case 157: { if (IPOP() >  0) BRANCH16(); else pc += 3; break; } /* ifgt */
        case 158: { if (IPOP() <= 0) BRANCH16(); else pc += 3; break; } /* ifle */
        case 159: { s4 b = IPOP(), a = IPOP();
            if (a == b) BRANCH16(); else pc += 3; break; }      /* if_icmpeq */
        case 160: { s4 b = IPOP(), a = IPOP();
            if (a != b) BRANCH16(); else pc += 3; break; }      /* if_icmpne */
        case 161: { s4 b = IPOP(), a = IPOP();
            if (a <  b) BRANCH16(); else pc += 3; break; }      /* if_icmplt */
        case 162: { s4 b = IPOP(), a = IPOP();
            if (a >= b) BRANCH16(); else pc += 3; break; }      /* if_icmpge */
        case 163: { s4 b = IPOP(), a = IPOP();
            if (a >  b) BRANCH16(); else pc += 3; break; }      /* if_icmpgt */
        case 164: { s4 b = IPOP(), a = IPOP();
            if (a <= b) BRANCH16(); else pc += 3; break; }      /* if_icmple */
        case 165: { slot b = POP(), a = POP();
            if (a == b) BRANCH16(); else pc += 3; break; }      /* if_acmpeq */
        case 166: { slot b = POP(), a = POP();
            if (a != b) BRANCH16(); else pc += 3; break; }      /* if_acmpne */
        case 167: BRANCH16(); break;                            /* goto */
        case 170: {                                             /* tableswitch */
            u4 base = (pc + 4) & ~3u;
            s4 def, lo, hi, v;
            u4 count;
            /* header: default + low + high = 12 bytes past the pad */
            if (base > m->code_len || m->code_len - base < 12)
                die("truncated tableswitch header");
            def = rd_s4(code + base);
            lo  = rd_s4(code + base + 4);
            hi  = rd_s4(code + base + 8);
            if (hi < lo) die("bad tableswitch (high < low)");
            count = (u4)hi - (u4)lo + 1;        /* number of jump offsets */
            if (count == 0 || count > (m->code_len - (base + 12)) / 4)
                die("truncated tableswitch table");
            v = IPOP();
            if (v < lo || v > hi) pc = (u4)((s4)pc + def);
            else pc = (u4)((s4)pc +
                           rd_s4(code + base + 12 + (u4)(v - lo) * 4));
            break; }
        case 171: {                                             /* lookupswitch */
            u4 base = (pc + 4) & ~3u;
            s4 def, n, v, i, off;
            /* header: default + npairs = 8 bytes past the pad */
            if (base > m->code_len || m->code_len - base < 8)
                die("truncated lookupswitch header");
            def = rd_s4(code + base);
            n   = rd_s4(code + base + 4);
            if (n < 0 || (u4)n > (m->code_len - (base + 8)) / 8)
                die("truncated lookupswitch table");
            v   = IPOP();
            off = def;
            for (i = 0; i < n; i++) {
                if (rd_s4(code + base + 8 + (u4)i * 8) == v) {
                    off = rd_s4(code + base + 12 + (u4)i * 8);
                    break;
                }
            }
            pc = (u4)((s4)pc + off);
            break; }
        case 172: case 176:                                     /* ireturn, areturn */
            *retval = POP(); *has_ret = 1;
            free(locals); free(stack); depth--; return;
        case 177:                                               /* return */
            *has_ret = 0;
            free(locals); free(stack); depth--; return;
        case 178: {                                             /* getstatic */
            cp_t *fref;
            code_need(m, pc, 3);
            fref = cp_get(C, rd_u2(code + pc + 1), CP_FIELDREF, "getstatic");
            cp_t *cls  = cp_get(C, fref->a, CP_CLASS, "getstatic class");
            cp_t *nat  = cp_get(C, fref->b, CP_NAMEANDTYPE, "getstatic nat");
            if (utf8_eq(C, cls->a, "java/lang/System") &&
                utf8_eq(C, nat->a, "out"))
                PUSH(REF_STDOUT);
            else if (utf8_eq(C, cls->a, "java/lang/System") &&
                     utf8_eq(C, nat->a, "err"))
                PUSH(REF_STDERR);
            else
                die("getstatic: only java.lang.System.out/err supported");
            pc += 3; break; }
        case 182: {                                             /* invokevirtual */
            cp_t *mref, *cls, *nat;
            int is_println, is_print;
            code_need(m, pc, 3);
            mref = cp_get(C, rd_u2(code + pc + 1), CP_METHODREF,
                          "invokevirtual");
            cls  = cp_get(C, mref->a, CP_CLASS, "iv class");
            nat  = cp_get(C, mref->b, CP_NAMEANDTYPE, "iv nat");
            if (!utf8_eq(C, cls->a, "java/io/PrintStream"))
                die("invokevirtual: only java.io.PrintStream print/println "
                    "supported (no objects in this VM)");
            is_println = utf8_eq(C, nat->a, "println");
            is_print   = utf8_eq(C, nat->a, "print");
            if (!is_println && !is_print)
                die("invokevirtual: only print/println supported");
            if (utf8_eq(C, nat->b, "(I)V")) {
                s4 v = IPOP(); FILE *f = stream_of(POP());
                fprintf(f, "%d", (int)v);
                if (is_println) fputc('\n', f);
            } else if (utf8_eq(C, nat->b, "(C)V")) {
                s4 v = IPOP(); FILE *f = stream_of(POP());
                fputc((int)(v & 0xff), f);
                if (is_println) fputc('\n', f);
            } else if (utf8_eq(C, nat->b, "(Z)V")) {
                s4 v = IPOP(); FILE *f = stream_of(POP());
                fputs(v ? "true" : "false", f);
                if (is_println) fputc('\n', f);
            } else if (utf8_eq(C, nat->b, "(Ljava/lang/String;)V")) {
                slot v = POP(); FILE *f = stream_of(POP());
                native_print_str(C, f, v, is_println);
            } else if (utf8_eq(C, nat->b, "()V")) {
                FILE *f = stream_of(POP());
                if (is_println) fputc('\n', f);
            } else {
                die("print/println overload not supported "
                    "(hint: no string concatenation; print pieces separately)");
            }
            pc += 3; break; }
        case 184:                                               /* invokestatic */
            code_need(m, pc, 3);
            invoke_static(C, rd_u2(code + pc + 1), stack, &sp);
            pc += 3; break;
        case 188: {                                             /* newarray */
            u1 atype;
            s4 count;
            code_need(m, pc, 2);
            atype = code[pc + 1];
            count = IPOP();
            if (atype != 10)   /* T_INT */
                die("newarray: only int[] supported");
            PUSH(array_new(count));
            pc += 2; break; }
        case 190: {                                             /* arraylength */
            int a = array_deref(POP());
            PUSH(g_arr_len[a]); pc++; break; }
        case 196: {                                             /* wide */
            u1 wop;
            u2 idx;
            code_need(m, pc, 4);
            wop = code[pc + 1];
            idx = rd_u2(code + pc + 2);
            if (wop == 21 || wop == 25) { PUSH(LGET(idx)); pc += 4; }
            else if (wop == 54 || wop == 58) { LGET(idx) = POP(); pc += 4; }
            else if (wop == 132) {
                code_need(m, pc, 6);
                LGET(idx) = (slot)((s4)LGET(idx) + rd_s2(code + pc + 4));
                pc += 6;
            } else die_op("unsupported wide opcode", wop, (int)pc);
            break; }
        case 198: { if (POP() == REF_NULL) BRANCH16(); else pc += 3; break; }
        case 199: { if (POP() != REF_NULL) BRANCH16(); else pc += 3; break; }
        case 200:                                               /* goto_w */
            code_need(m, pc, 5);
            pc = (u4)((s4)pc + rd_s4(code + pc + 1)); break;
        default:
            die_op("unsupported opcode (outside the mjvm subset)",
                   op, (int)pc);
        }
    }
#undef PUSH
#undef POP
#undef IPOP
#undef LGET
#undef BRANCH16
}

/* ---- main ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
    class_t C;
    method_t *m;
    slot args[1], rv;
    int has_ret;
    cp_t *this_cls;

    printf("mjvm " MJVM_VERSION
           " micro-JVM (RustChain Amiga Edition, java/ Phase 3)\n");
    if (argc != 2) {
        printf("usage: mjvm <ClassFile.class>\n");
        printf("runs static main of a single class; int+String subset, "
               "Java 8 bytecode or older\n");
        return 20;
    }

    class_load(&C, argv[1]);
    this_cls = cp_get(&C, C.this_class, CP_CLASS, "this class");
    printf("mjvm: loaded ");
    utf8_print(&C, this_cls->a);
    printf(" (format %d.x, %d methods, %d cp entries)\n",
           (int)C.major, (int)C.method_count, (int)C.cp_count);

    m = find_method(&C, "main", "([Ljava/lang/String;)V");
    if (!m) die("no public static void main(String[]) in class");
    if (!(m->flags & 0x0008)) die("main is not static");

    printf("mjvm: --- program output ---\n");
    args[0] = REF_NULL;   /* String[] args: null (args.length unsupported) */
    rv = 0; has_ret = 0;
    run(&C, m, args, 1, &rv, &has_ret);
    printf("mjvm: --- exit OK ---\n");
    return 0;
}
