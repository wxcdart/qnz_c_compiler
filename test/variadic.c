#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src = 
        "extern int printf(const char *, ...);\n"
        "#define PRINT(fmt, ...) printf(fmt, __VA_ARGS__)\n"
        "#define MSG(msg) PRINT(\"%s\\n\", msg)\n"
        "#define EMPTY() \n"
        "int main(void) {\n"
        "    PRINT(\"Hello %s\\n\", \"world\");\n"
        "    MSG(\"foo\");\n"
        "    PRINT(\"Single arg: %d\\n\", 42);\n"
        "    PRINT(\"No variadic: %d\\n\", 100); /* __VA_ARGS__ empty */\n"
        "    return 0;\n"
        "}\n";
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    fprintf(stderr, "Preprocessed:\n%s\n", prep);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    
    int result = zero_cc_execute_jit(comp, NULL);
    
    // We expect the program to print something and return 0.
    // We'll just check that execution didn't crash.
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 0 ? 0 : 1;
}