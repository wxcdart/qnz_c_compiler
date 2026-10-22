#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "_Static_assert(sizeof(_Atomic int) == sizeof(int), \"atomic size\");\n"
        "_Atomic int counter = 0;\n"
        "int main(void) { return 42; }\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) {
        perror("calloc");
        return 1;
    }
    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        fprintf(stderr, "Preprocessing failed\n");
        free(comp);
        return 1;
    }
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
               zero_cc_compiler_free(comp);
        return 1;
    }
    int result = zero_cc_execute_jit(comp, NULL);
    printf("Atomic JIT result: %d (expected 42)\n", result);
    zero_cc_compiler_free(comp);
    return result == 42 ? 0 : 1;
}
