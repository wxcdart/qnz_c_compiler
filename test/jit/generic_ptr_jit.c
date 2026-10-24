#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    /* Test 1: int* matches int* (not void*) */
    const char *src1 =
        "int main(void) {"
        "int i = 42;"
        "return _Generic((&i), int*: 3, void*: 2, default: 0);"
        "}";
    ZeroCCCompiler *comp1 = calloc(1, sizeof(ZeroCCCompiler));
    char *prep1 = zero_cc_preprocess(src1, NULL);
    zero_cc_tokenize_c99(prep1, comp1);
    free(prep1);
    zero_cc_compile_c99(comp1);
    if (comp1->had_error) {
        fprintf(stderr, "Compile error test1: %s\n", comp1->error_msg);
        return 1;
    }
    int result1 = zero_cc_execute_jit(comp1, NULL);
    zero_cc_compiler_free(comp1);
    printf("Test 1 (int* matches int*): %d (expected 3)\n", result1);
    if (result1 != 3) return 1;

    /* Test 2: void* matches void* */
    const char *src2 =
        "int main(void) {"
        "void *p = 0;"
        "return _Generic((p), void*: 3, int*: 2, default: 0);"
        "}";
    ZeroCCCompiler *comp2 = calloc(1, sizeof(ZeroCCCompiler));
    char *prep2 = zero_cc_preprocess(src2, NULL);
    zero_cc_tokenize_c99(prep2, comp2);
    free(prep2);
    zero_cc_compile_c99(comp2);
    if (comp2->had_error) {
        fprintf(stderr, "Compile error test2: %s\n", comp2->error_msg);
        return 1;
    }
    int result2 = zero_cc_execute_jit(comp2, NULL);
    zero_cc_compiler_free(comp2);
    printf("Test 2 (void* matches void*): %d (expected 3)\n", result2);
    if (result2 != 3) return 1;

    /* Test 3: int* does NOT match void* (falls to default) */
    const char *src3 =
        "int main(void) {"
        "int i = 42;"
        "return _Generic((&i), void*: 3, default: 42);"
        "}";
    ZeroCCCompiler *comp3 = calloc(1, sizeof(ZeroCCCompiler));
    char *prep3 = zero_cc_preprocess(src3, NULL);
    zero_cc_tokenize_c99(prep3, comp3);
    free(prep3);
    zero_cc_compile_c99(comp3);
    if (comp3->had_error) {
        fprintf(stderr, "Compile error test3: %s\n", comp3->error_msg);
        return 1;
    }
    int result3 = zero_cc_execute_jit(comp3, NULL);
    zero_cc_compiler_free(comp3);
    printf("Test 3 (int* != void* -> default): %d (expected 42)\n", result3);
    if (result3 != 42) return 1;

    printf("All _Generic pointer tests passed!\n");
    return 0;
}