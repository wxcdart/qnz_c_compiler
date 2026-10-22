/* Negative tests: C99 6.7.5.3p6 — a call whose function type has a
 * prototype must have a matching argument count.  Invalid programs below
 * must be REJECTED (had_error set); the control cases (correct arity, and
 * a no-prototype empty-paren declaration called with arguments — which is
 * legal C99) must be ACCEPTED.
 *
 * Regression for the silent miscompile where add(1) / add(1,2,3) on a
 * 2-parameter function compiled and ran with garbage. */
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

    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return rc;
}

int main(void) {
    int fails = 0;

    /* 1. Too few arguments to a 2-parameter prototype */
    fails += run_case(
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return add(1); }\n",
        1);

    /* 2. Too many arguments to a 2-parameter prototype */
    fails += run_case(
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return add(1, 2, 3); }\n",
        1);

    /* 3. (void) prototype is a prototype: zero parameters only */
    fails += run_case(
        "int f(void) { return 0; }\n"
        "int main(void) { return f(1); }\n",
        1);

    /* 4. Variadic prototype: at least the named parameters are required */
    fails += run_case(
        "int fv(int a, ...) { return a; }\n"
        "int main(void) { return fv(); }\n",
        1);

    /* 5. Prototype declared separately (not a definition) */
    fails += run_case(
        "int g(int a, int b);\n"
        "int main(void) { return g(1); }\n",
        1);

    /* 6. Control: correct arity must compile */
    fails += run_case(
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return add(1, 2); }\n",
        0);

    /* 7. Control: (void) called with no arguments must compile */
    fails += run_case(
        "int f(void) { return 7; }\n"
        "int main(void) { return f(); }\n",
        0);

    /* 8. Control: variadic with enough arguments must compile */
    fails += run_case(
        "int fv(int a, ...) { return a; }\n"
        "int main(void) { return fv(5, 6, 7); }\n",
        0);

    /* 9. Control: empty-paren () is NOT a prototype — calling with any
     * argument count is legal C99 (no constraint applies), even though
     * the callee is never defined (compile-time check only). */
    fails += run_case(
        "int h();\n"
        "int main(void) { return h(1, 2); }\n",
        0);

    if (fails) {
        fprintf(stderr, "%d call-arity case(s) failed\n", fails);
        return 1;
    }
    printf("PASS: call-arity constraint checking (9/9 cases)\n");
    return 0;
}
