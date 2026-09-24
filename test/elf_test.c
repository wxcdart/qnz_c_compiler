#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int foo(void) { return 123; }\n"
        "int main(void) { return foo(); }\n";
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
    const char *outfile = "test.o";
    bool success = zero_cc_emit_elf_obj(comp, outfile);
    if (success) {
        printf("ELF object emitted to %s\n", outfile);
    } else {
        fprintf(stderr, "Failed to emit ELF object\n");
    }
    /* Cleanup */
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return success ? 0 : 1;
}
