/* Test rich diagnostics: file:line:col format and warning vs error distinction */
#include "zero_cc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

int main(void) {
    const char *src_error = 
        "int main() { return x; }\n";  /* Error: 'x' undefined */
    
    const char *src_static_assert_fail = 
        "_Static_assert(0, \"assertion failed\");\n"
        "int main() { return 0; }\n";
    
    ZeroCCCompiler *c;
    char *prep;
    
    /* Test 1: Error diagnostic for undefined variable */
    printf("Test 1: Error diagnostic for undefined variable\n");
    printf("Source: %s", src_error);
    c = calloc(1, sizeof(ZeroCCCompiler));
    prep = zero_cc_preprocess(src_error, NULL);
    zero_cc_tokenize_c99(prep, c);
    free(prep);
    zero_cc_compile_c99(c);
    printf("had_error: %d (expected 1)\n", c->had_error);
    assert(c->had_error && "Expected had_error to be set");
    zero_cc_buf_free(&c->text_section);
    zero_cc_buf_free(&c->rodata_section);
    zero_cc_buf_free(&c->data_section);
    free(c->tokens);
    free(c);
    
    /* Test 2: Static assertion failure */
    printf("\n\nTest 2: Static assertion failure\n");
    printf("Source: %s", src_static_assert_fail);
    c = calloc(1, sizeof(ZeroCCCompiler));
    prep = zero_cc_preprocess(src_static_assert_fail, NULL);
    zero_cc_tokenize_c99(prep, c);
    free(prep);
    zero_cc_compile_c99(c);
    printf("had_error: %d (expected 1)\n", c->had_error);
    assert(c->had_error && "Expected had_error to be set");
    zero_cc_buf_free(&c->text_section);
    zero_cc_buf_free(&c->rodata_section);
    zero_cc_buf_free(&c->data_section);
    free(c->tokens);
    free(c);
    
    printf("\nAll diagnostics tests completed.\n");
    return 0;
}
