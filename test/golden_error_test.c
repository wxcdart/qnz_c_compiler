#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

int main(void) {
    const char *src = "int main(void) { break; }";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    assert(comp != NULL);
    char *prep = zero_cc_preprocess(src, NULL);
    assert(prep != NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    // Expect a semantic error to have been detected
    assert(comp->had_error);
    // Cleanup
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return 0;
}
