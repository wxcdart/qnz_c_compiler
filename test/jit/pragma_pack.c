/* Test #pragma pack functionality */
#include <stdio.h>
#include <stdlib.h>
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
    /* Test 1: #pragma pack(1) without push/pop */
    {
        const char *src =
            "#pragma pack(1)\n"
            "struct Packed1 { char a; int b; };\n"
            "int main(void) { return sizeof(struct Packed1); }\n";
        int res = run_snippet(src);
        printf("Test 1 (pack 1): %d (expected 5)\n", res);
        if (res != 5) {
            fprintf(stderr, "FAILED: Test 1\n");
            return 1;
        }
    }
    
    /* Test 2: #pragma pack(push, 1) / pop */
    {
        const char *src =
            "#pragma pack(push, 1)\n"
            "struct Packed2 { char a; int b; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Packed2); }\n";
        int res = run_snippet(src);
        printf("Test 2 (push 1, pop): %d (expected 5)\n", res);
        if (res != 5) {
            fprintf(stderr, "FAILED: Test 2\n");
            return 1;
        }
    }
    
    /* Test 3: #pragma pack(4) */
    {
        const char *src =
            "#pragma pack(4)\n"
            "struct Packed4 { char a; int b; };\n"
            "int main(void) { return sizeof(struct Packed4); }\n";
        int res = run_snippet(src);
        printf("Test 3 (pack 4): %d (expected 8)\n", res);
        if (res != 8) {
            fprintf(stderr, "FAILED: Test 3\n");
            return 1;
        }
    }
    
    /* Test 4: Nested pack values */
    {
        const char *src =
            "#pragma pack(push, 1)\n"
            "struct Outer { char a; struct Inner { char x; int y; }; };\n"
            "#pragma pack(pop)\n"
            "int main(void) { return sizeof(struct Outer) + sizeof(struct Inner); }\n";
        int res = run_snippet(src);
        printf("Test 4 (nested pack): %d (expected 11)\n", res);
        if (res != 11) {
            fprintf(stderr, "FAILED: Test 4\n");
            return 1;
        }
    }
    
    /* Test 5: _Pragma("pack(1)") syntax */
    {
        const char *src =
            "_Pragma(\"pack(1)\")\n"
            "struct Packed5 { char a; int b; };\n"
            "int main(void) { return sizeof(struct Packed5); }\n";
        int res = run_snippet(src);
        printf("Test 5 (_Pragma pack 1): %d (expected 5)\n", res);
        if (res != 5) {
            fprintf(stderr, "FAILED: Test 5\n");
            return 1;
        }
    }
    
    /* Test 6: #pragma pack() with no arguments (reset to default) */
    {
        const char *src =
            "#pragma pack(1)\n"
            "#pragma pack()\n"
            "struct Packed6 { char a; int b; };\n"
            "int main(void) { return sizeof(struct Packed6); }\n";
        int res = run_snippet(src);
        printf("Test 6 (pack reset): %d (expected 8)\n", res);
        if (res != 8) {
            fprintf(stderr, "FAILED: Test 6\n");
            return 1;
        }
    }
    
    printf("All pragma pack tests passed!\n");
    return 0;
}
