#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "int main(void) {\n"
        "    return (int)2.5;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    /* Print the generated code */
    printf("Text section size: %zu\n", comp->text_section.length);
    printf("Rodata section size: %zu\n", comp->rodata_section.length);
    
    /* Disassemble first 100 bytes */
    printf("\nGenerated code (first 100 bytes):\n");
    for (size_t i = 0; i < comp->text_section.length && i < 100; i++) {
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
