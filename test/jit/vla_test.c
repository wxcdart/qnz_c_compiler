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
    printf("Running VLA test suite...\n");

    /* Test 1: Basic int VLA */
    {
        const char *src =
            "int run_vla(int n) {\n"
            "    int arr[n];\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        arr[i] = i * 2;\n"
            "    }\n"
            "    int sum = 0;\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        sum += arr[i];\n"
            "    }\n"
            "    return sum;\n"
            "}\n"
            "int main(void) {\n"
            "    return run_vla(5);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (int VLA sum): %d (expected 20)\n", res);
        assert(res == 20);
    }

    /* Test 2: Char VLA */
    {
        const char *src =
            "int run_char_vla(int n) {\n"
            "    char buf[n];\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        buf[i] = 10 + i;\n"
            "    }\n"
            "    int sum = 0;\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        sum += buf[i];\n"
            "    }\n"
            "    return sum;\n"
            "}\n"
            "int main(void) {\n"
            "    return run_char_vla(4);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 2 (char VLA sum): %d (expected 46)\n", res);
        assert(res == 46);
    }

    /* Test 3: VLA with fixed-size array (hybrid VLA rule: only one runtime VLA) */
    {
        const char *src =
            "int run_multi_vla(int n, int m) {\n"
            "    int a[n];\n"           /* Runtime VLA */
            "    int b[4];\n"           /* Fixed-size array (comptime) */
            "    for (int i = 0; i < n; i++) a[i] = i + 1;\n"
            "    for (int j = 0; j < m; j++) b[j] = (j + 1) * 10;\n"
            "    return a[n - 1] + b[m - 1];\n"
            "}\n"
            "int main(void) {\n"
            "    return run_multi_vla(3, 4);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 3 (VLA + fixed array): %d (expected 43)\n", res);
        assert(res == 43);
    }

    /* Test 4: Passing VLA to function (pointer decay) */
    {
        const char *src =
            "int sum_ptr(int *p, int count) {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < count; i++) {\n"
            "        total += p[i];\n"
            "    }\n"
            "    return total;\n"
            "}\n"
            "int run_pass_vla(int n) {\n"
            "    int arr[n];\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        arr[i] = i + 1;\n"
            "    }\n"
            "    return sum_ptr(arr, n);\n"
            "}\n"
            "int main(void) {\n"
            "    return run_pass_vla(5);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 4 (VLA pointer decay): %d (expected 15)\n", res);
        assert(res == 15);
    }

    /* Test 5: Compound assignment on VLA elements */
    {
        const char *src =
            "int run_compound_vla(int n) {\n"
            "    int arr[n];\n"
            "    for (int i = 0; i < n; i++) {\n"
            "        arr[i] = 10;\n"
            "    }\n"
            "    arr[0] += 5;\n"
            "    arr[1] -= 3;\n"
            "    arr[2] *= 2;\n"
            "    return arr[0] + arr[1] + arr[2];\n"
            "}\n"
            "int main(void) {\n"
            "    return run_compound_vla(3);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 5 (VLA compound assign): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 6: Fixed size array indexing and LEA decay */
    {
        const char *src =
            "int sum_fixed(int *p, int count) {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < count; i++) {\n"
            "        total += p[i];\n"
            "    }\n"
            "    return total;\n"
            "}\n"
            "int main(void) {\n"
            "    int arr[4];\n"
            "    arr[0] = 10;\n"
            "    arr[1] = 20;\n"
            "    arr[2] = 30;\n"
            "    arr[3] = 40;\n"
            "    return sum_fixed(arr, 4);\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 6 (fixed array & decay): %d (expected 100)\n", res);
        assert(res == 100);
    }

    printf("All VLA tests passed!\n");
    return 0;
}
