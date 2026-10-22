#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int main(void) {\n"
        "    double a = 10.0;\n"
        "    a -= 2.5; /* 10.0 - 2.5 = 7.5 */\n"
        "    a /= 2.5; /* 7.5 / 2.5 = 3.0 */\n"
        "    a *= 4.0; /* 3.0 * 4.0 = 12.0 */\n"
        "    a += 0.5; /* 12.0 + 0.5 = 12.5 */\n"
        "    return (int)a;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 12)\n", ret);
    zero_cc_compiler_free(comp);
    return ret == 12 ? 0 : 1;
}
