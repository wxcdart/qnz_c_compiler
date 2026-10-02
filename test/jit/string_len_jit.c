#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int main(void) {\n"
        "    const char *s = \"hello\";\n"
        "    int len = 0;\n"
        "    while (s[len] != 0) {\n"
        "        len++;\n"
        "    }\n"
        "    return len == 5 ? 1 : 0;\n"
        "}\n";
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
    fprintf(stderr, "DEBUG: before tokenize\n");
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    fprintf(stderr, "DEBUG: after tokenize, token_count=%zu\n", comp->token_count);
    for (size_t i = 0; i < comp->token_count; i++) {
        fprintf(stderr, "  token[%zu]: type=%d str='%s'\n", i, comp->tokens[i].type, comp->tokens[i].str_val);
    }
    fprintf(stderr, "DEBUG: before compile\n");
    zero_cc_compile_c99(comp);
    fprintf(stderr, "DEBUG: after compile\n");
    fprintf(stderr, "DEBUG: text_section.length=%zu\n", comp->text_section.length);
    fprintf(stderr, "DEBUG: rodata_section.length=%zu\n", comp->rodata_section.length);
    fprintf(stderr, "DEBUG: str_reloc_count=%zu\n", comp->str_reloc_count);
    for (size_t i = 0; i < comp->str_reloc_count; i++) {
        fprintf(stderr, "DEBUG:   str_reloc[%zu]: rodata_off=%zu text_patch_off=%zu\n",
                i, comp->str_relocs[i].rodata_offset, comp->str_relocs[i].text_patch_offset);
    }
    fprintf(stderr, "DEBUG: before execute\n");
    int result = zero_cc_execute_jit(comp, NULL);
    fprintf(stderr, "DEBUG: execute result=%d\n", result);
    printf("JIT result: %d\n", result);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 1 ? 0 : 1;
}
