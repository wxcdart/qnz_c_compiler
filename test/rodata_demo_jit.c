#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

/*
 * Demonstrates that string literals are placed in .rodata and accessed via RIP‑relative LEA.
 * The JIT‐compiled program prints "hello world" and returns 0.
 */
int main(void) {
    const char *src = "#include <stdio.h>\nint main(void) { printf(\"hello world\\n\"); return 0; }";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}
