#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc.h"

int main(void) {
    // Define first BEFORE main to ensure proper registration
    const char *src = 
        "typedef void* va_list;\n"
        "#define va_arg(ap, type) __builtin_va_arg(ap, type)\n"
        "#define va_end(ap) ((void)0)\n"
        "int first(int n, ...) {\n"
        "    va_list args;\n"
        "    va_start(args, n);\n"
        "    int v = __builtin_va_arg(args, int);\n"
        "    va_end(args);\n"
        "    return v;\n"
        "}\n"
        "int main(void) {\n"
        "    return first(1, 42);\n"
        "}\n";
    
    printf("Source:\n%s\n", src);
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    printf("Preprocessed:\n%s\n", prep);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    
    printf("Token count: %zu\n", comp->token_count);
    
    zero_cc_compile_c99(comp);
    
    printf("Function count after compile: %zu\n", comp->function_count);
    for (size_t i = 0; i < comp->function_count; i++) {
        printf("  [%zu] %s at offset %zu\n", i, comp->functions[i].name, comp->functions[i].text_offset);
    }
    
    int result = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 42)\n", result);
    
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 42 ? 0 : 1;
}
