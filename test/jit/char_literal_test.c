#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

/* Regression: char literals in expressions (call args, return, comparisons)
 * were unhandled by the AST expression parser and hung or failed to parse. */
static int run_snippet(const char *src) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}

static int check(const char *name, const char *src, int expect) {
    int got = run_snippet(src);
    if (got != expect) {
        printf("FAIL %s: expected %d, got %d\n", name, expect, got);
        return 1;
    }
    return 0;
}

int main(void) {
    int fails = 0;
    printf("Running char literal test suite...\n");
    fails += check("call arg",
        "int f(int a) { return a; }\nint main(void) { return f('a'); }\n", 97);
    fails += check("return",
        "int main(void) { return 'X'; }\n", 88);
    fails += check("escape",
        "int f(int a) { return a; }\nint main(void) { return f('\\n'); }\n", 10);
    fails += check("compare",
        "int main(void) { int c = 'b'; if (c == 'b') return 1; return 0; }\n", 1);
    if (fails == 0) printf("All char literal tests passed\n");
    return fails ? 1 : 0;
}
