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
    
    // Debug: print functions
    for (size_t i = 0; i < comp->function_count; i++) {
        ZeroCCFunction *fn = &comp->functions[i];
        printf("Function: %s, offset=%zu, size=%zu, returns_struct=%d\n", 
            fn->name, fn->text_offset, fn->ret_struct_sz, fn->returns_struct);
    }
    
    // Debug: print struct info
    for (size_t i = 0; i < comp->struct_count; i++) {
        ZeroCCStructDef *sd = &comp->structs[i];
        printf("Struct: %s, size=%zu, align=%zu, members=%zu\n", 
            sd->name, sd->type.size, sd->type.align, sd->type.member_count);
        for (size_t j = 0; j < sd->type.member_count; j++) {
            ZeroCCStructMember *m = &sd->type.members[j];
            printf("  Member: %s, offset=%zu, size=%zu, align=%zu, tag=%d, is_flexible=%d\n", 
                m->name, m->offset, m->size, m->align, m->tag, m->is_flexible);
        }
    }
    
    // Debug: print locals
    for (size_t i = 0; i < comp->local_count; i++) {
        ZeroCCSym *sym = &comp->locals[i];
        printf("Local: %s, tag=%d, size=%zu, offset=%d\n", 
            sym->name, sym->type.tag, sym->type.size, sym->stack_offset);
    }
    
    // Debug: print text section
    printf("Text section size: %zu\n", comp->text_section.length);
    for (size_t i = 0; i < comp->text_section.length && i < 64; i++) {
        printf("%02x ", ((uint8_t*)comp->text_section.data)[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
    
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return 0;
}