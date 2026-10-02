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
    
    // Debug: print tokens
    printf("Tokens:\n");
    for (size_t i = 0; i < comp->token_count; i++) {
        printf("  token[%zu]: type=%d str='%s'\n", i, comp->tokens[i].type, comp->tokens[i].str_val);
    }
    printf("\n");
    
    // Manually invoke parser up to function body
    // First, skip past 'int main(void) {' 
    // The tokens are:
    // token[12]: type=4 str='int'        - return type
    // token[13]: type=3 str='main'       - function name
    // token[14]: type=57 str=''          - '('
    // token[14]: type=6 str='void'       - void parameter
    // token[15]: type=58 str=''          - ')'
    // token[16]: type=55 str=''          - '{'
    // token[16]: type=7 str='struct'     - struct declaration
    // ...
    
    printf("About to call zero_cc_compile_c99...\n");
    zero_cc_compile_c99(comp);
    
    // Debug: print functions
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        printf("Function: %s, offset=%zu, size=%zu, returns_struct=%d\n", 
            fn->name, fn->text_offset, fn->ret_struct_sz, fn->returns_struct);
    }
    
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return 0;
}