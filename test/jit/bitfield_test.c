#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "struct BasicBits {\n"
        "    unsigned int a : 3;\n"
        "    unsigned int b : 5;\n"
        "    unsigned int c : 8;\n"
        "};\n"
        "\n"
        "struct OverflowBits {\n"
        "    unsigned int a : 20;\n"
        "    unsigned int b : 20;\n"
        "};\n"
        "\n"
        "struct SignedBits {\n"
        "    int s4 : 4;\n"
        "    unsigned int u4 : 4;\n"
        "};\n"
        "\n"
        "struct MixedStruct {\n"
        "    int prefix;\n"
        "    unsigned int b1 : 4;\n"
        "    unsigned int b2 : 4;\n"
        "    int suffix;\n"
        "};\n"
        "\n"
        "int test_sizes(void) {\n"
        "    if (sizeof(struct BasicBits) != 4) return 1;\n"
        "    if (sizeof(struct OverflowBits) != 8) return 2;\n"
        "    if (sizeof(struct SignedBits) != 4) return 3;\n"
        "    if (sizeof(struct MixedStruct) != 12) return 4;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_unsigned_values(void) {\n"
        "    struct BasicBits bb;\n"
        "    bb.a = 5;\n"
        "    bb.b = 25;\n"
        "    bb.c = 200;\n"
        "    if (bb.a != 5) return (int)bb.a;\n"
        "    if (bb.b != 25) return 11;\n"
        "    if (bb.c != 200) return (int)bb.c;\n"
        "    /* Modify only one field and ensure others remain untouched */\n"
        "    bb.b = 7;\n"
        "    if (bb.a != 5) return 13;\n"
        "    if (bb.b != 7) return 14;\n"
        "    if (bb.c != 200) return 15;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_overflow_values(void) {\n"
        "    struct OverflowBits ob;\n"
        "    ob.a = 100000;\n"
        "    ob.b = 200000;\n"
        "    if (ob.a != 100000) return (int)ob.a;\n"
        "    if (ob.b != 200000) return 21;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_signed_values(void) {\n"
        "    struct SignedBits sb;\n"
        "    sb.s4 = -1;\n"
        "    sb.u4 = 15;\n"
        "    if (sb.s4 != -1) return (int)sb.s4;\n"
        "    if (sb.u4 != 15) return 31;\n"
        "    sb.s4 = -7;\n"
        "    if (sb.s4 != -7) return 32;\n"
        "    sb.s4 = 5;\n"
        "    if (sb.s4 != 5) return 33;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_mixed_struct(void) {\n"
        "    struct MixedStruct ms;\n"
        "    ms.prefix = 42;\n"
        "    ms.b1 = 9;\n"
        "    ms.b2 = 14;\n"
        "    ms.suffix = 99;\n"
        "    if (ms.prefix != 42) return 40;\n"
        "    if (ms.b1 != 9) return 41;\n"
        "    if (ms.b2 != 14) return 42;\n"
        "    if (ms.suffix != 99) return 43;\n"
        "    /* Test compound assignment on bitfield */\n"
        "    ms.b1 += 3;\n"
        "    if (ms.b1 != 12) return 44;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    int r = test_sizes();\n"
        "    if (r != 0) return r;\n"
        "    r = test_unsigned_values();\n"
        "    if (r != 0) return r;\n"
        "    r = test_overflow_values();\n"
        "    if (r != 0) return r;\n"
        "    r = test_signed_values();\n"
        "    if (r != 0) return r;\n"
        "    r = test_mixed_struct();\n"
        "    if (r != 0) return r;\n"
        "    return 0;\n"
        "}\n";

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);

    if (comp->had_error) {
        fprintf(stderr, "Compilation error: %s\n", comp->error_msg);
        return 1;
    }

    int ret = zero_cc_execute_jit(comp, NULL);
    printf("Bitfield JIT result: %d\n", ret);
    if (ret == 0) {
        printf("PASS\n");
    } else {
        printf("FAIL: error code %d\n", ret);
    }

    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);

    return ret == 0 ? 0 : 1;
}
