#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

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

int main(void) {
    printf("Running C99 inline test suite...\n");

    /* Test 1: static inline function */
    {
        const char *src =
            "static inline int square(int x) {\n"
            "    return x * x;\n"
            "}\n"
            "int main(void) {\n"
            "    return square(7);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (static inline): %d (expected 49)\n", res);
        assert(res == 49);
    }

    /* Test 2: inline function without static */
    {
        const char *src =
            "inline int add(int a, int b) {\n"
            "    return a + b;\n"
            "}\n"
            "int main(void) {\n"
            "    return add(10, 20);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 2 (inline func): %d (expected 30)\n", res);
        assert(res == 30);
    }

    /* Test 3: Multiple inline specifiers or mixed static/inline */
    {
        const char *src =
            "inline static int mul(int a, int b) {\n"
            "    return a * b;\n"
            "}\n"
            "int main(void) {\n"
            "    return mul(6, 7);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 3 (inline static): %d (expected 42)\n", res);
        assert(res == 42);
    }

    printf("All inline tests passed!\n");
    return 0;
}
