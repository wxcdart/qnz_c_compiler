#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "int main(void) {\n"
        "    float a[4]; float b[4]; float c[4];\n"
        "    a[0] = 1.0f; a[1] = 2.0f; a[2] = 3.0f; a[3] = 4.0f;\n"
        "    b[0] = 10.0f; b[1] = 20.0f; b[2] = 30.0f; b[3] = 40.0f;\n"
        "    for (int i = 0; i < 4; i++) {\n"
        "        c[i] = a[i] + b[i];\n"
        "    }\n"
        "    if (c[0] != 11.0f) return 1;\n"
        "    if (c[1] != 22.0f) return 2;\n"
        "    if (c[2] != 33.0f) return 3;\n"
        "    if (c[3] != 44.0f) return 4;\n"
        "    return 0;\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    comp->dump_ir = 0;
    zero_cc_compile_c99(comp);
    int res = zero_cc_execute_jit(comp, NULL);
    return res;
}
