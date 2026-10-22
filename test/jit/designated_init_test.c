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
               zero_cc_compiler_free(comp);
        return -1;
    }
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_compiler_free(comp);
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

    /* Test 5: Zero-initialization of omitted gaps in arrays and structs */
    {
        const char *src =
            "struct Gap { int a; int b; int c; };\n"
            "int main(void) {\n"
            "    struct Gap g = { .b = 42 };\n"
            "    int arr[6] = { [2] = 40, [5] = 2 };\n"
            "    if (g.a != 0 || g.c != 0) return 1;\n"
            "    if (arr[0] != 0 || arr[1] != 0 || arr[3] != 0 || arr[4] != 0) return 2;\n"
            "    return g.b + arr[0] + arr[1];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 6: Array designated initializer out-of-bounds rejected */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[3] = { [5] = 10 };\n"
            "    return 0;\n"
            "}\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        assert(comp->had_error != 0);
        assert(strstr(comp->error_msg, "exceeds array bound") != NULL);
               zero_cc_compiler_free(comp);
    }

    /* Test 7: VLA with initializer rejected */
    {
        const char *src =
            "int main(void) {\n"
            "    int n = 5;\n"
            "    int vla[n] = { 1, 2, 3 };\n"
            "    return 0;\n"
            "}\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        assert(comp->had_error != 0);
        assert(strstr(comp->error_msg, "cannot be initialized") != NULL);
               zero_cc_compiler_free(comp);
    }

    printf("All Designated / Brace Initializer JIT tests passed!\n");
    return 0;
}
