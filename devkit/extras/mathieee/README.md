# mathieee replacement libraries

`mathieeedoubbas.library` and `mathieeedoubtrans.library`, written from
scratch in m68k assembly (MIT, Elyan Labs 2026). FPU-backed: doubles come
in as D0/D1(+D2/D3) register pairs per the classic API, get pushed through
FP0, results return in D0/D1.

Why: vbcc's native vasm assembler opens both libraries at startup
(softfloat vclib). Workbench ships them; the bare AROS m68k ROM only has
mathieeesingbas, so on the RustChain AROS image vasm exits silently
without them. These fill the gap. Requires an FPU (68881/68882/040/060 or
emulator; FS-UAE's A4000/040 qualifies).

Build: `./build_host.sh [vamos-path]` assembles them with the devkit's own
native vasm+vlink running under vamos (amitools). Output in `build/`,
installed copies in `devkit/aros-libs/` and the test volume `libs/`.

API coverage: the full documented function table for both libraries
(doubbas: Fix/Flt/Cmp/Tst/Abs/Neg/Add/Sub/Mul/Div/Floor/Ceil; doubtrans:
Atan/Sin/Cos/Tan/Sincos/Sinh/Cosh/Tanh/Exp/Log/Pow/Sqrt/Tieee/Fieee/
Asin/Acos/Log10). Register conventions verified against the official .fd
files. Transcendentals use 6888x instructions; on a real 040/060 those
need the OS 680x0.library FPSP traps (standard on any SetPatch'd system),
under FS-UAE they are emulated directly.

Tested: loaded via LIBS: on the AROS ROM boot; vasm 1.9 opened both and
assembled the devkit's hello.c and these very libraries' sources.
