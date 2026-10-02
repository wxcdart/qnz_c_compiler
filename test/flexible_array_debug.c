#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "struct flex { int len; char data[]; };\n"
        "int main(void) {\n"
        "    struct flex f;\n"
        "    f.len = 5;\n"
        "    return f.len;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    // Debug: print text section
    printf("Text section size: %zu\n", comp->text_section.length);
    for (size_t i = 0; i < comp->text_section.length && i < 200; i++) {
        printf("%02x ", ((uint8_t*)comp->text_section.data)[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    
    // Debug: print functions
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        printf("Function: %s, offset=%zu, size=%zu, returns_struct=%d\n", 
            fn->name, fn->text_offset, fn->ret_struct_sz, fn->returns_struct);
    }
    
    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Result: %d (expected 5)\n", ret);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret == 5 ? 0 : 1;
}