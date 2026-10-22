#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int main(void) {\n"
        "    double a = 2.5;\n"
        "    double b = 3.0;\n"
        "    double c = a + b;\n"
        "    return 0;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 0)\n", ret);
    zero_cc_compiler_free(comp);
    return ret == 0 ? 0 : 1;
}
