#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "int main(void) {"
        "int i = 42;"
        "return _Generic((&i), void*: 3, int: 1, default: 0);"
        "}";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        return 1;
    }
    int result = zero_cc_execute_jit(comp, NULL);
    printf("Generic ptr JIT result: %d (expected 3)\n", result);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 3 ? 0 : 1;
}
