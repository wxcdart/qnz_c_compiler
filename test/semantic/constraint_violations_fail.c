/* Negative tests: constraint violations that were silently miscompiled
 * before the C99 constraint-checking pass (JIT and AOT alike).
 *
 *  1. C99 6.8.4.3p1 — a goto's label must be defined in the enclosing
 *     function (was: compiled and ran, jumping to a bogus target).
 *  2. C99 6.5.2.1p3  — a struct member must exist (was: compiled and the
 *     write landed at offset 0, corrupting the first field).
 *  3. C99 6.8p10     — a block's closing '}' is required (was: truncated
 *     source compiled and ran; AOT only failed later at link time).
 *
 * Control cases (valid goto / member access / balanced braces) must stay
 * accepted.  Call-argument-count constraints (C99 6.7.5.3) are covered
 * separately in call_arity_fail.c. */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

static int run_case(const char *src, int expect_error) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) { perror("calloc"); return 1; }

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) { fprintf(stderr, "Preprocess failed\n"); return 1; }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    zero_cc_compile_c99(comp);

    int rc;
    if (expect_error && !comp->had_error) {
        fprintf(stderr, "FAIL: expected a compile error but compilation succeeded\n");
        fprintf(stderr, "      source: %s\n", src);
        rc = 1;
    } else if (!expect_error && comp->had_error) {
        fprintf(stderr, "FAIL: unexpected compile error: %s\n", comp->error_msg);
        fprintf(stderr, "      source: %s\n", src);
        rc = 1;
    } else {
        rc = 0;
    }

    zero_cc_compiler_free(comp);
    return rc;
}

int main(void) {
    int fails = 0;

    /* 1. goto an undefined label */
    fails += run_case(
        "int main(void) { goto nowhere; return 0; }\n",
        1);

    /* 2. forward goto to a defined label (must compile) */
    fails += run_case(
        "int main(void) { int x = 1; if (x) goto done; x = 2; done: return x; }\n",
        0);

    /* 3. backward goto (loop idiom, must compile) */
    fails += run_case(
        "int main(void) { int i = 0; loop: i++; if (i < 3) goto loop; return i; }\n",
        0);

    /* 4. struct member that does not exist */
    fails += run_case(
        "struct P { int x; int y; };\n"
        "int main(void) { struct P p; p.x = 1; p.y2 = 2; return p.x; }\n",
        1);

    /* 5. valid member and arrow access (must compile) */
    fails += run_case(
        "struct P { int x; int y; };\n"
        "int main(void) { struct P p; p.x = 1; p.y = 2; return p.x + p.y; }\n",
        0);

    /* 6. function body truncated before its closing brace */
    fails += run_case(
        "int f(void) { if (1) { return 1; } return 2;\n",
        1);

    /* 7. control: complete function (must compile) */
    fails += run_case(
        "int f(void) { if (1) { return 1; } return 2; }\n"
        "int main(void) { return f(); }\n",
        0);

    if (fails) {
        fprintf(stderr, "%d constraint-violation case(s) failed\n", fails);
        return 1;
    }
    printf("PASS: constraint-violation diagnostics (7/7 cases)\n");
    return 0;
}
