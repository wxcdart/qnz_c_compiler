/* zero_rt zero_san report (Sprint 167): the UBSan-style trap diagnostic.
 *
 * Called on the trap path of every zero_san check when the user compiles
 * with -fsanitize-trap (GCC/Clang-compatible: checks report, then trap).
 * The compiler inserts, at each OP_OVF_TRAP site that would otherwise be
 * a bare ud2/brk/unimp:
 *
 *     <op, width, checked value into the ABI integer arg registers>
 *     call __zerocsan_report
 *     <bare trap>
 *
 * so the message reaches stderr before the trap signal kills the process.
 * The function RETURNS; the bare trap instruction after the call does the
 * killing, which keeps the per-ISA trap signal (SIGILL/SIGTRAP) as the
 * process death. The call site is on the fatal path only, so no register
 * or stack discipline beyond the normal call ABI is required.
 *
 * Emission is stdio-free (see san_fmt.h): the message is formatted into a
 * local buffer and written raw via zeroc_rt_write(2, ...). This keeps the
 * zero_rt stdio members (which define stdout/stderr/stdin) OUT of
 * host-linked AOT builds, where they would collide with the system libc.
 * The only zero_rt member this pulls in is os.o (zeroc_rt_*-prefixed, no
 * libc collisions).
 *
 * Argument values:
 *   - op 0/1/2 (signed overflow add/sub/mul): `v` is the raw result in
 *     the checked register - on 64-bit ISAs the exact 64-bit sum/product
 *     for a width-4 check (the operands are sign-extended) and the
 *     wrapped 64-bit value for width 8; on rv32 (no width-8 checks) the
 *     sign-extended 32-bit result.
 *   - op 3/4 (div/rem by zero): the message is value-free (UBSan parity);
 *     `v` (the zero divisor) is not printed.
 *   - op 5 (shift count): `v` is the offending count, printed contracted
 *     to the check width.
 *
 * Link mode (OKF sanitizer-roadmap.md, "runtime follows link mode"):
 *   - JIT --run:  the driver links a host-compiled copy
 *     (zero_rt/jit/san_report_host.c -> jit_san_report.o, the
 *     JIT_INT128_OBJ pattern) and the JIT resolves the call via
 *     dlsym(RTLD_DEFAULT) on the executable's symbol table.
 *   - AOT -o:     the zero_rt_<isa>.a fallback archive (already on the
 *     system/cross link line) supplies the member.
 *   - --zerocc-link: zero_rt.a is auto-linked.
 *
 * The op codes below MUST stay in sync with ZC_OVF_OP_* in
 * src/zero_cc/isa/isa_backend.h.
 */
#include <stdint.h>
#include "san_fmt.h"
#include "rt_os.h"

#define ZCSAN_OP_ADD          0
#define ZCSAN_OP_SUB          1
#define ZCSAN_OP_MUL          2
#define ZCSAN_OP_DIV_ZERO     3
#define ZCSAN_OP_REM_ZERO     4
#define ZCSAN_OP_SHIFT_COUNT  5

void __zerocsan_report(unsigned op, unsigned width, long v) {
    char buf[128];
    char *p = buf;
    long sv = v;
    if (width == 4) sv = (long)(int32_t)v;  /* count: contract to check width */

    if (op == ZCSAN_OP_ADD || op == ZCSAN_OP_SUB || op == ZCSAN_OP_MUL) {
        p = san_fmt_str(p, "zerocc: runtime error: signed integer overflow: ");
        p = san_fmt_long(p, v);
        p = san_fmt_str(p, width == 8
                        ? " cannot be represented in a 'long'\n"
                        : " cannot be represented in an 'int'\n");
    } else if (op == ZCSAN_OP_DIV_ZERO) {
        p = san_fmt_str(p, "zerocc: runtime error: division by zero\n");
    } else if (op == ZCSAN_OP_REM_ZERO) {
        p = san_fmt_str(p, "zerocc: runtime error: remainder by zero\n");
    } else if (op == ZCSAN_OP_SHIFT_COUNT) {
        p = san_fmt_str(p, "zerocc: runtime error: ");
        if (sv < 0) {
            p = san_fmt_str(p, "negative shift exponent: ");
            p = san_fmt_long(p, sv);
        } else {
            p = san_fmt_str(p, "shift exponent ");
            p = san_fmt_long(p, sv);
            p = san_fmt_str(p, " is too large for ");
            p = san_fmt_uint(p, width * 8U);
            p = san_fmt_str(p, "-bit type '");
            p = san_fmt_str(p, width == 8 ? "long" : "int");
            p = san_fmt_str(p, "'");
        }
        *p++ = '\n';
    } else {
        p = san_fmt_str(p, "zerocc: runtime error: unknown zero_san check ");
        p = san_fmt_uint(p, op);
        p = san_fmt_str(p, "\n");
    }
    zeroc_rt_write(2, buf, (uint64_t)(p - buf));
}
