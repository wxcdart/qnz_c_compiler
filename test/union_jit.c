#include <stdio.h>
#include <stdlib.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "typedef union {\n"
        "    int i;\n"
        "    char c[4];\n"
        "} IntBytes;\n"
        "\n"
        "typedef union {\n"
        "    char a;\n"
        "    int b;\n"
        "    long c;\n"
        "} Overlap;\n"
        "\n"
        "typedef union {\n"
        "    char c[5];\n"
        "    int i;\n"
        "} PaddedUnion;\n"
        "\n"
        "union TaggedUnion {\n"
        "    long l;\n"
        "    int arr[2];\n"
        "};\n"
        "\n"
        "int test_sizes(void) {\n"
        "    if (sizeof(IntBytes) != 4) return 1;\n"
        "    if (sizeof(Overlap) != 8) return 2;\n"
        "    if (sizeof(PaddedUnion) != 8) return 3;\n"
        "    if (sizeof(union TaggedUnion) != 8) return 4;\n"
        "    if (_Alignof(IntBytes) != 4) return 5;\n"
        "    if (_Alignof(Overlap) != 8) return 6;\n"
        "    if (_Alignof(PaddedUnion) != 4) return 7;\n"
        "    if (_Alignof(union TaggedUnion) != 8) return 8;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_overlap(void) {\n"
        "    Overlap ov;\n"
        "    ov.c = 0x12345678;\n"
        "    if (ov.b != 0x12345678) return 10;\n"
        "    ov.b = 42;\n"
        "    if (ov.c != 42) return 11;\n"
        "    if (ov.a != 42) return 12;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_tagged(void) {\n"
        "    union TaggedUnion tu;\n"
        "    tu.l = 99;\n"
        "    if (tu.l != 99) return 20;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    int r = test_sizes();\n"
        "    if (r != 0) return r;\n"
        "    r = test_overlap();\n"
        "    if (r != 0) return r;\n"
        "    r = test_tagged();\n"
        "    if (r != 0) return r;\n"
        "    return 0;\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return 1;

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        free(comp);
        return 1;
    }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);

    int result = zero_cc_execute_jit(comp, NULL);
    printf("Union JIT result: %d\n", result);

    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);

    if (result == 0) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAIL: error code %d\n", result);
        return 1;
    }
}
