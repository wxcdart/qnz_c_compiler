#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

int main(void) {
    printf("Running static function test...\n");

    /* Test: static function */
    {
        const char *src =
            "static int square(int x) {\n"
            "    return x * x;\n"
            "}\n"
            "int main(void) {\n"
            "    return square(7);\n"
            "}\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        int res = zero_cc_execute_jit(comp, NULL);
        zero_cc_compiler_free(comp);
        printf("Test (static func): %d (expected 49)\n", res);
        assert(res == 49);
    }

    printf("Static function test passed!\n");
    return 0;
}
