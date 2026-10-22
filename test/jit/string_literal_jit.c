#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "int main(void) {"
        "const char *s = \"hello\";"
        "return s != 0;"
        "}";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return 1;
    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) return 1;
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int result = zero_cc_execute_jit(comp, NULL);
    printf("string_literal result: %d\n", result);
    zero_cc_compiler_free(comp);
    return result == 1 ? 0 : 1;
}
