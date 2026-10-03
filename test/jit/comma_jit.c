#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

static int run_snippet(const char *src) {
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
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}

int main(void) {
    printf("Running Comma Operator JIT test...\n");

    /* Test 1: Basic chained comma expression */
    {
        const char *src =
            "int main(void) {\n"
            "    int a = (1, 2, 3, 42);\n"
            "    return a;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 2: Side effects executed in sequence */
    {
        const char *src =
            "int main(void) {\n"
            "    int x = 0;\n"
            "    int y = (x = x + 10, x = x * 2, x + 5);\n"
            "    return y;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 25);
    }

    /* Test 3: Comma expression statement & assignment precedence */
    {
        const char *src =
            "int main(void) {\n"
            "    int x = 0;\n"
            "    int y = 0;\n"
            "    x = 10, y = 20;\n"
            "    return x + y;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 30);
    }

    /* Test 4: Comma in for-loop init and step */
    {
        const char *src =
            "int main(void) {\n"
            "    int sum = 0;\n"
            "    int i = 0;\n"
            "    int j = 0;\n"
            "    for (i = 0, j = 10; i < 5; i++, j--) {\n"
            "        sum = sum + i + j;\n"
            "    }\n"
            "    return sum;\n"
            "}\n";
        int res = run_snippet(src);
        /* i: 0..4 (sum 10), j: 10, 9, 8, 7, 6 (sum 40) => 50 */
        assert(res == 50);
    }

    /* Test 5: Function call with parenthesized comma argument */
    {
        const char *src =
            "int add(int a, int b) {\n"
            "    return a + b;\n"
            "}\n"
            "int main(void) {\n"
            "    int x = 1;\n"
            "    return add((x = x + 4, x), 10);\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 15);
    }

    printf("All Comma Operator JIT tests passed!\n");
    return 0;
}
