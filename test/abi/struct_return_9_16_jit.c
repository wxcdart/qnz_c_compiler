#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "typedef struct { long a; int b; } St12;\n"
        "St12 make_st12(long a, int b) {\n"
        "    St12 s;\n"
        "    s.a = a;\n"
        "    s.b = b;\n"
        "    return s;\n"
        "}\n"
        "int main(void) {\n"
        "    St12 res = make_st12(100, 42);\n"
        "    if (res.a != 100) return 1;\n"
        "    if (res.b != 42) return 2;\n"
        "    return 0;\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return 1;

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        free(comp);
        return 1;
    }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);

    int result = zero_cc_execute_jit(comp, NULL);
    printf("Struct 12-byte return JIT result: %d\n", result);

    zero_cc_compiler_free(comp);

    if (result == 0) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: %d\n", result);
        return 1;
    }
}
