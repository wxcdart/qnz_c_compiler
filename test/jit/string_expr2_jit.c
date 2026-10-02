#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = "int main(void) { \"a\"; return 1; }";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int result = zero_cc_execute_jit(comp, NULL);
    printf("result %d\n", result);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 1 ? 0 : 1;
}
