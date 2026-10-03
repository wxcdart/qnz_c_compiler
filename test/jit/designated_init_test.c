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
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        free(comp->tokens);
        free(comp);
        return -1;
    }
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}

int main(void) {
    printf("Running Designated / Brace Initializer JIT tests...\n");

    /* Test 1: Struct designated initializer */
    {
        const char *src =
            "struct Point { int x; int y; };\n"
            "int main(void) {\n"
            "    struct Point p = { .y = 30, .x = 12 };\n"
            "    return p.x + p.y;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 2: Struct positional brace initializer */
    {
        const char *src =
            "struct Vector { int a; int b; int c; };\n"
            "int main(void) {\n"
            "    struct Vector v = { 10, 20, 12 };\n"
            "    return v.a + v.b + v.c;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 3: Array positional brace initializer */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[4] = { 5, 10, 15, 12 };\n"
            "    return arr[0] + arr[1] + arr[2] + arr[3];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 4: Array designated initializer with indices */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[5] = { [1] = 20, [3] = 22 };\n"
            "    return arr[1] + arr[3];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    printf("All Designated / Brace Initializer JIT tests passed!\n");
    return 0;
}
