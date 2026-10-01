#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int test_func(int a, double b, int c, double d) {\n"
        "    int sum = a + c;\n"
        "    double dsum = b + d;\n"
        "    return sum + (int)dsum;\n"
        "}\n"
        "int main(void) {\n"
        "    return test_func(10, 2.5, 20, 3.5);\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 36 = 10 + 20 + (int)(2.5 + 3.5))\n", ret);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret == 36 ? 0 : 1;
}