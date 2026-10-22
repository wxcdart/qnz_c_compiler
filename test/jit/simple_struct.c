#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "struct simple { int a; int b; };\n"
        "int main(void) {\n"
        "    struct simple f;\n"
        "    f.a = 5;\n"
        "    return f.a;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 5)\n", ret);
    zero_cc_compiler_free(comp);
    return ret == 5 ? 0 : 1;
}