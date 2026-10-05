// Simple test to reproduce constant folding bug
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
    printf("Testing constant folding bug...\n");
    
    // Test 1: Simple constant fold (should work)
    {
        const char *src =
            "int main(void) {\n"
            "    int x = 2 + 3;\n"
            "    return x;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (2 + 3): %d (expected 5)\n", res);
        assert(res == 5);
    }
    
    // Test 2: VLA with constant arithmetic (triggers bug)
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
        printf("Test 2 (VLA sum): %d (expected 20)\n", res);
        assert(res == 20);
    }
    
    printf("All tests passed!\n");
    return 0;
}
