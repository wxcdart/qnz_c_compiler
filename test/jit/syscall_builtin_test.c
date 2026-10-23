/* JIT test: __zerocc_syscall builtin (zero_rt Phase 0 OS boundary).
 *
 * Compiles embedded programs through the full pipeline and executes them
 * in-memory. The inner programs exercise the syscall intrinsic directly:
 *   - write(1, ...) to stdout, result checked against the byte count
 *   - the return value flows into the program exit code
 * Negative cases: 0 arguments and 8 arguments must be compile errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc.h"

static int compile_and_run(const char *src, char *err, size_t errsz) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return -1;
    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        free(comp);
        return -1;
    }
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        snprintf(err, errsz, "%s", comp->error_msg);
        zero_cc_compiler_free(comp);
        return -2;
    }
    int result = zero_cc_execute_jit(comp, NULL);
    zero_cc_compiler_free(comp);
    return result;
}

int main(void) {
    /* write(1, "zerocc-syscall-ok\n", 18) via the builtin; check the
     * kernel's return value (bytes written) end-to-end. */
    const char *src =
        "int main(void) {\n"
        "    const char *msg = \"zerocc-syscall-ok\\n\";\n"
        "    long rc = __zerocc_syscall(1, 1, msg, 18);\n"
        "    if (rc != 18) return 1;\n"
        "    return 0;\n"
        "}\n";
    char err[512] = {0};
    int rc = compile_and_run(src, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "syscall builtin: expected success, got rc=%d (%s)\n", rc, err);
        return 1;
    }

    /* 6-argument syscall: writev-free - use fstat? No: keep it simple and
     * portable - brk(0) returns the current program break (must be > 0). */
    const char *src6 =
        "int main(void) {\n"
        "    long brk = __zerocc_syscall(12, 0, 0, 0, 0, 0, 0);\n"
        "    if (brk <= 0) return 1;\n"
        "    return 0;\n"
        "}\n";
    rc = compile_and_run(src6, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "6-arg syscall (brk): expected success, got rc=%d (%s)\n", rc, err);
        return 1;
    }

    /* Negative: no syscall number */
    const char *bad0 = "int main(void) { __zerocc_syscall(); return 0; }\n";
    rc = compile_and_run(bad0, err, sizeof(err));
    if (rc != -2) {
        fprintf(stderr, "0-arg syscall: expected compile error, got rc=%d\n", rc);
        return 1;
    }
    if (strstr(err, "__zerocc_syscall") == NULL) {
        fprintf(stderr, "0-arg syscall: unexpected error message: %s\n", err);
        return 1;
    }

    /* Negative: too many arguments */
    const char *bad8 = "int main(void) { __zerocc_syscall(1,2,3,4,5,6,7,8); return 0; }\n";
    rc = compile_and_run(bad8, err, sizeof(err));
    if (rc != -2) {
        fprintf(stderr, "8-arg syscall: expected compile error, got rc=%d\n", rc);
        return 1;
    }
    if (strstr(err, "at most 7") == NULL) {
        fprintf(stderr, "8-arg syscall: unexpected error message: %s\n", err);
        return 1;
    }

    printf("__zerocc_syscall: JIT write + brk + arg-count errors ok\n");
    return 0;
}
