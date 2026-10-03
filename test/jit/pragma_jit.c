/* Test pragma support: pack, message, GCC diagnostic */
#include <stdio.h>
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
    printf("Running pragma test suite...\n");

    /* Test 1: #pragma pack(push, 1) */
    {
        const char *src =
            "#pragma pack(push, 1)\n"
            "struct Packed1 { char a; int b; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Packed1); }\n";
        int res = run_snippet(src);
        printf("Test 1 (pack 1): %d (expected 5)\n", res);
        assert(res == 5);
    }

    /* Test 2: #pragma pack(push, 2) */
    {
        const char *src =
            "#pragma pack(push, 2)\n"
            "struct Packed2 { char a; int b; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Packed2); }\n";
        int res = run_snippet(src);
        printf("Test 2 (pack 2): %d (expected 6)\n", res);
        assert(res == 6);
    }

    /* Test 3: #pragma pack(push) without value */
    {
        const char *src =
            "#pragma pack(push)\n"
            "struct Packed3 { char a; int b; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Packed3); }\n";
        int res = run_snippet(src);
        printf("Test 3 (pack push/pop): %d (expected 8, default alignment)\n", res);
        /* Default alignment is typically 8 for int */
        assert(res == 8);
    }

    /* Test 4: #pragma pack(n) syntax */
    {
        const char *src =
            "#pragma pack(4)\n"
            "struct Packed4 { char a; int b; };\n"
            "int main(void) { return sizeof(struct Packed4); }\n";
        int res = run_snippet(src);
        printf("Test 4 (pack 4): %d (expected 8)\n", res);
        assert(res == 8);
    }

    /* Test 5: Nested pack push/pop */
    {
        const char *src =
            "#pragma pack(push, 1)\n"
            "struct Outer { char a; int b; };\n"
            "#pragma pack(push, 2)\n"
            "struct Inner { char x; int y; };\n"
            "#pragma pack(pop)\n"
            "struct Outer2 { char c; int d; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Outer) + sizeof(struct Inner) + sizeof(struct Outer2); }\n";
        int res = run_snippet(src);
        printf("Test 5 (nested pack): %d (expected 5+6+5=16, Outer2 still under pack(1))\n", res);
        assert(res == 16);
    }

    printf("All pragma tests passed!\n");
    return 0;
}
