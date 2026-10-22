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
    zero_cc_compiler_free(comp);
    return ret;
}

int main(void) {
    printf("Running restrict test suite...\n");

    /* Test 1: store/forward across a store through a different restrict pointer */
    {
        const char *src =
            "void f(long *restrict a, long *restrict b, long *restrict c) {\n"
            "    *a = 10;\n"
            "    *b = 20;\n"
            "    *c = *a;\n"
            "}\n"
            "int main(void) {\n"
            "    long x = 0;\n" "    long y = 0;\n" "    long z = 0;\n"
            "    f(&x, &y, &z);\n"
            "    return (int)(x + y + z);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (distinct restrict ptrs): %d (expected 40)\n", res);
        assert(res == 40);
    }

    /* Test 2: unrestricted aliasing must NOT be forwarded across */
    {
        const char *src =
            "void g(long *a, long *b, long *c) {\n"
            "    *a = 10;\n"
            "    *b = 20;\n"
            "    *c = *a;\n"
            "}\n"
            "int main(void) {\n"
            "    long x = 0;\n" "    long z = 0;\n"
            "    g(&x, &x, &z);\n"
            "    return (int)z;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 2 (aliasing, no restrict): %d (expected 20)\n", res);
        assert(res == 20);
    }

    /* Test 3: repeated loads through restrict pointer */
    {
        const char *src =
            "long h(long *restrict a, long *restrict b) {\n"
            "    long s = *a + *a;\n"
            "    *b = 5;\n"
            "    return s + *a;\n"
            "}\n"
            "int main(void) {\n"
            "    long x = 7;\n" "    long y = 0;\n"
            "    return (int)h(&x, &y);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 3 (reload after other restrict store): %d (expected 21)\n", res);
        assert(res == 21);
    }

    printf("All restrict tests passed!\n");
    return 0;
}
