#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    // Test variadic function with va_start/va_arg
    const char *src = 
        "#include <stdarg.h>\n"
        "int sum(int count, ...) {\n"
        "    va_list args;\n"
        "    va_start(args, count);\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < count; i++) {\n"
        "        total += va_arg(args, int);\n"
        "    }\n"
        "    va_end(args);\n"
        "    return total;\n"
        "}\n"
        "int main(void) {\n"
        "    return sum(3, 1, 2, 3) == 6 ? 0 : 1;\n"
        "}\n";
    
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    FILE *f = fopen("/tmp/text_fn.bin", "wb");
    if (f) {
        fwrite(comp->text_section.data, 1, comp->text_section.length, f);
        fclose(f);
    }
    int result = zero_cc_execute_jit(comp, NULL);
    printf("variadic_fn result: %d (expected 0)\n", result);
    
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 0 ? 0 : 1;
}
