/* Host-side __zerocsan_report for the zerocc driver's --run JIT (Sprint 167).
 *
 * The JIT resolves `__zerocsan_report` via dlsym(RTLD_DEFAULT) against the
 * driver's own symbol table (the JIT_INT128_OBJ pattern), so the driver
 * must carry its own copy of the reporter, compiled by the HOST compiler
 * (ZC_CC) rather than zerocc. Everything except the final write is shared
 * with the zero_rt copy (zero_rt/core/san_report.c) via san_fmt.h - the
 * messages are byte-identical, and the output goes to fd 2 through the
 * host libc's write() (the driver is statically linked with musl, which
 * provides it).
 *
 * The op codes MUST stay in sync with ZC_OVF_OP_* in
 * src/zero_cc/isa/isa_backend.h (and with zero_rt/core/san_report.c).
 */
#include <stdint.h>
#include <unistd.h>
#include "../core/san_fmt.h"

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
    (void)write(2, buf, (size_t)(p - buf));
}
