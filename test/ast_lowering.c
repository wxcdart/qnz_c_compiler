#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int test_arithmetic_and_return(void) {
    const char *src =
        "int main(void) {\n"
        "    int a = 10;\n"
        "    int b = 3;\n"
        "    int c = a + b * 2 - (a / b);\n"
        "    return c;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Arithmetic test compile error: %s\n", comp->error_msg);
        return 0;
    }
    int res = zero_cc_execute_jit(comp, NULL);
    // 10 + 3*2 - (10/3) = 10 + 6 - 3 = 13
    if (res != 13) {
        fprintf(stderr, "Arithmetic test expected 13, got %d\n", res);
        return 0;
    }
    return 1;
}

int test_if_else(void) {
    const char *src =
        "int main(void) {\n"
        "    int x = 20;\n"
        "    int y = 0;\n"
        "    if (x > 15) {\n"
        "        y = 100;\n"
        "    } else {\n"
        "        y = 200;\n"
        "    }\n"
        "    return y;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "If-else test compile error: %s\n", comp->error_msg);
        return 0;
    }
    int res = zero_cc_execute_jit(comp, NULL);
    if (res != 100) {
        fprintf(stderr, "If-else test expected 100, got %d\n", res);
        return 0;
    }
    return 1;
}

int test_while_loop(void) {
    const char *src =
        "int main(void) {\n"
        "    int sum = 0;\n"
        "    int i = 1;\n"
        "    while (i <= 5) {\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return sum;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "While loop test compile error: %s\n", comp->error_msg);
        return 0;
    }
    int res = zero_cc_execute_jit(comp, NULL);
    // 1 + 2 + 3 + 4 + 5 = 15
    if (res != 15) {
        fprintf(stderr, "While loop test expected 15, got %d\n", res);
        return 0;
    }
    return 1;
}

int test_early_return(void) {
    const char *src =
        "int main(void) {\n"
        "    int val = 42;\n"
        "    if (val == 42) {\n"
        "        return 77;\n"
        "    }\n"
        "    return 99;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Early return test compile error: %s\n", comp->error_msg);
        return 0;
    }
    int res = zero_cc_execute_jit(comp, NULL);
    if (res != 77) {
        fprintf(stderr, "Early return test expected 77, got %d\n", res);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!test_arithmetic_and_return()) {
        fprintf(stderr, "Arithmetic test FAILED\n");
        return 1;
    }
    if (!test_if_else()) {
        fprintf(stderr, "If-else test FAILED\n");
        return 1;
    }
    if (!test_while_loop()) {
        fprintf(stderr, "While loop test FAILED\n");
        return 1;
    }
    if (!test_early_return()) {
        fprintf(stderr, "Early return test FAILED\n");
        return 1;
    }
    printf("AST lowering tests passed\n");
    return 0;
}
