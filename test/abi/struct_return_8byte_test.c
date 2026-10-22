#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "typedef struct { int x; int y; } Point;\n"
        "Point create_point(int x, int y) {\n"
        "    Point p;\n"
        "    p.x = x;\n"
        "    p.y = y;\n"
        "    int a = 1; int b = 2; int c = 3; int d = 4;\n"
        "    int e = a + b + c + d;\n"
        "    return p;\n"
        "}\n"
        "int main(void) {\n"
        "    Point pt = create_point(10, 20);\n"
        "    if (pt.x != 10) return 1;\n"
        "    if (pt.y != 20) return 2;\n"
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
    printf("Struct 8-byte return JIT result: %d\n", result);

    zero_cc_compiler_free(comp);

    if (result == 0) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: %d\n", result);
        return 1;
    }
}
