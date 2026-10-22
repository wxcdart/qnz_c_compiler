#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "long test_func(long a, int b) {\n"
        "    return a + b;\n"
        "}\n"
        "int main(void) {\n"
        "    return (int)test_func(100, 42);\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 142)\n", ret);
    zero_cc_compiler_free(comp);
    return ret == 142 ? 0 : 1;
}