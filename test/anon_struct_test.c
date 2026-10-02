#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

int main(void) {
    const char *src =
        "struct AnonUnionInStruct {\n"
        "    int tag;\n"
        "    union {\n"
        "        int ival;\n"
        "        double dval;\n"
        "    };\n"
        "    int end_marker;\n"
        "};\n"
        "\n"
        "union AnonStructInUnion {\n"
        "    unsigned int raw;\n"
        "    struct {\n"
        "        short lo;\n"
        "        short hi;\n"
        "    };\n"
        "};\n"
        "\n"
        "struct DeeplyNested {\n"
        "    int id;\n"
        "    union {\n"
        "        struct {\n"
        "            int a;\n"
        "            int b;\n"
        "        };\n"
        "        long full;\n"
        "    };\n"
        "    int trailer;\n"
        "};\n"
        "\n"
        "struct AnonWithBitfields {\n"
        "    int header;\n"
        "    union {\n"
        "        unsigned int val;\n"
        "        struct {\n"
        "            unsigned int b1 : 3;\n"
        "            unsigned int b2 : 5;\n"
        "            unsigned int b3 : 8;\n"
        "        };\n"
        "    };\n"
        "    int footer;\n"
        "};\n"
        "\n"
        "struct NamedNestedStruct {\n"
        "    struct {\n"
        "        int x;\n"
        "        int y;\n"
        "    } pt;\n"
        "    int z;\n"
        "};\n"
        "\n"
        "int test_anon_union(void) {\n"
        "    struct AnonUnionInStruct u;\n"
        "    u.tag = 10;\n"
        "    u.ival = 42;\n"
        "    u.end_marker = 99;\n"
        "    if (u.tag != 10) return 1;\n"
        "    if (u.ival != 42) return 2;\n"
        "    if (u.end_marker != 99) return 3;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_anon_struct(void) {\n"
        "    union AnonStructInUnion as;\n"
        "    as.lo = 0x12;\n"
        "    as.hi = 0x34;\n"
        "    if (as.lo != 0x12) return 11;\n"
        "    if (as.hi != 0x34) return 12;\n"
        "    if (as.raw != (0x00340012)) return 13;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_deeply_nested(void) {\n"
        "    struct DeeplyNested dn;\n"
        "    dn.id = 100;\n"
        "    dn.a = 55;\n"
        "    dn.b = 77;\n"
        "    dn.trailer = 200;\n"
        "    if (dn.id != 100) return 21;\n"
        "    if (dn.a != 55) return 22;\n"
        "    if (dn.b != 77) return 23;\n"
        "    if (dn.trailer != 200) return 24;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_anon_bitfields(void) {\n"
        "    struct AnonWithBitfields ab;\n"
        "    ab.header = 111;\n"
        "    ab.val = 0;\n"
        "    ab.b1 = 5;\n"
        "    ab.b2 = 17;\n"
        "    ab.b3 = 200;\n"
        "    ab.footer = 222;\n"
        "    if (ab.header != 111) return 31;\n"
        "    if (ab.b1 != 5) return 32;\n"
        "    if (ab.b2 != 17) return 33;\n"
        "    if (ab.b3 != 200) return 34;\n"
        "    if (ab.footer != 222) return 35;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int test_named_nested(void) {\n"
        "    struct NamedNestedStruct ns;\n"
        "    ns.pt.x = 10;\n"
        "    ns.pt.y = 20;\n"
        "    ns.z = 30;\n"
        "    if (ns.pt.x != 10) return 41;\n"
        "    if (ns.pt.y != 20) return 42;\n"
        "    if (ns.z != 30) return 43;\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "int main(void) {\n"
        "    int r;\n"
        "    r = test_anon_union();\n"
        "    if (r != 0) return r;\n"
        "    r = test_anon_struct();\n"
        "    if (r != 0) return r;\n"
        "    r = test_deeply_nested();\n"
        "    if (r != 0) return r;\n"
        "    r = test_anon_bitfields();\n"
        "    if (r != 0) return r;\n"
        "    r = test_named_nested();\n"
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
    printf("Anon struct JIT result: %d\n", ret);
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
