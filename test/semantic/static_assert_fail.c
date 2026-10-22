/* Negative test for _Static_assert - should fail to compile via zero_cc API */
#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "_Static_assert(0, \"This should fail\");\n"
        "int main(void) { return 0; }\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) { perror("calloc"); return 1; }

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) { fprintf(stderr, "Preprocess failed\n"); return 1; }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    zero_cc_compile_c99(comp);

    /* Expect compilation to fail */
    if (!comp->had_error) {
        fprintf(stderr, "FAIL: Expected compile error but compilation succeeded\n");
        zero_cc_compiler_free(comp);
        return 1;
    }

    /* Check error message is reasonable */
    if (comp->error_msg[0] == '\0') {
        fprintf(stderr, "FAIL: Expected error message but got empty string\n");
        zero_cc_compiler_free(comp);
        return 1;
    }

    printf("PASS: Correctly caught failing _Static_assert\n");
    printf("Error message: %s\n", comp->error_msg);

    zero_cc_compiler_free(comp);
    return 0;
}