#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int main(void) {\n"
        "    int line = __LINE__;\n"
        "    int line2 = __LINE__;\n"
        "    return line2 - line;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    int result = zero_cc_execute_jit(comp, NULL);
    
    printf("Line difference: %d (expected 1)\n", result);
    
    zero_cc_compiler_free(comp);
    return result == 1 ? 0 : 1;
}
