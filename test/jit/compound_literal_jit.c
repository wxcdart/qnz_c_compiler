#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "struct Point { int x; int y; };\n"
        "int sum_arr(int *arr, int len) {\n"
        "    int s = 0;\n"
        "    for (int i = 0; i < len; i++) s += arr[i];\n"
        "    return s;\n"
        "}\n"
        "int main(void) {\n"
        "    /* 1. Scalar compound literal */\n"
        "    int x = (int){42};\n"
        "    if (x != 42) return 1;\n"
        "\n"
        "    /* 2. Array compound literal decay to pointer */\n"
        "    int *p = (int[]){10, 20, 30};\n"
        "    if (p[0] != 10 || p[1] != 20 || p[2] != 30) return 2;\n"
        "\n"
        "    /* 3. Direct array subscripting */\n"
        "    int val = (int[]){100, 200, 300}[2];\n"
        "    if (val != 300) return 3;\n"
        "\n"
        "    /* 4. Address-of compound literal */\n"
        "    int *ptr = &(int){77};\n"
        "    if (*ptr != 77) return 4;\n"
        "\n"
        "    /* 5. Struct compound literal and member access */\n"
        "    int y = (struct Point){15, 25}.y;\n"
        "    if (y != 25) return 5;\n"
        "\n"
        "    /* 6. Struct designated initializer compound literal */\n"
        "    struct Point pt = (struct Point){.y = 50, .x = 30};\n"
        "    if (pt.x != 30 || pt.y != 50) return 6;\n"
        "\n"
        "    /* 7. Function argument passing */\n"
        "    int s = sum_arr((int[]){1, 2, 3, 4}, 4);\n"
        "    if (s != 10) return 7;\n"
        "\n"
        "    /* 8. Lvalue modification */\n"
        "    int *arr = (int[]){1, 2, 3};\n"
        "    arr[1] = 99;\n"
        "    if (arr[1] != 99) return 8;\n"
        "\n"
        "    return 42;\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        free(comp->tokens);
        free(comp);
        return 1;
    }
    int result = zero_cc_execute_jit(comp, NULL);
    printf("Compound literal comprehensive JIT result: %d (expected 42)\n", result);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return result == 42 ? 0 : 1;
}
