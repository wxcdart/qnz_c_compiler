#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "zero_cc.h"

/* C11 6.7.9: string-literal initialization of char arrays.
 *
 * Covers the shapes that the file-scope data-section path and the local
 * (stack) path each handle differently:
 *   - local char[] = "..." (unsized / sized / truncated / unsigned char)
 *   - local struct with a char-array member (positional + designated)
 *   - local 2-D char array rows
 *   - file-scope struct member + file-scope unsigned char[]
 *   - name reuse in sibling blocks (by-name local lookup is latest-wins,
 *     so type-dependent initializers must resolve their own symbol)
 *   - pointer-to-string control cases (must keep returning the address)
 */

static int run_snippet(const char *src) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) return -1;
    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) {
        free(comp);
        return -1;
    }
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    if (comp->had_error) {
        fprintf(stderr, "Compile error: %s\n", comp->error_msg);
        zero_cc_compiler_free(comp);
        return -1;
    }
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_compiler_free(comp);
    return ret;
}

int main(void) {
    printf("Running String-Literal Initializer JIT tests...\n");

    /* Test 1: unsized local char[] = "..." */
    {
        const char *src =
            "int main(void) {\n"
            "    char s[] = \"hello\";\n"
            "    if (s[0] != 'h' || s[4] != 'o' || s[5] != '\\0') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 2: sized local char[] shorter than the string room -> zero pad */
    {
        const char *src =
            "int main(void) {\n"
            "    char b[6] = \"ab\";\n"
            "    if (b[0] != 'a' || b[1] != 'b') return 1;\n"
            "    if (b[2] != '\\0' || b[3] != '\\0' || b[5] != '\\0') return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 3: truncation with no room for the NUL */
    {
        const char *src =
            "int main(void) {\n"
            "    char t[3] = \"abcdef\";\n"
            "    if (t[0] != 'a' || t[1] != 'b' || t[2] != 'c') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 4: unsigned char[] */
    {
        const char *src =
            "int main(void) {\n"
            "    unsigned char u[] = \"ab\";\n"
            "    if (u[0] != 'a' || u[1] != 'b' || u[2] != '\\0') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 5: escape sequences are copied as decoded bytes */
    {
        const char *src =
            "int main(void) {\n"
            "    char esc[] = \"a\\nb\";\n"
            "    if (esc[0] != 'a' || esc[1] != '\\n' || esc[2] != 'b' || esc[3] != '\\0') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 6: local struct with a char-array member (positional) */
    {
        const char *src =
            "struct P { int id; char name[8]; };\n"
            "int main(void) {\n"
            "    struct P p = { 42, \"hi\" };\n"
            "    if (p.id != 42) return 1;\n"
            "    if (p.name[0] != 'h' || p.name[1] != 'i' || p.name[2] != '\\0') return 2;\n"
            "    if (p.name[3] != '\\0' || p.name[7] != '\\0') return 3;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 7: local struct char-array member (designated) */
    {
        const char *src =
            "struct P { int id; char name[8]; };\n"
            "int main(void) {\n"
            "    struct P d = { .id = 7, .name = \"yo\" };\n"
            "    if (d.id != 7) return 1;\n"
            "    if (d.name[0] != 'y' || d.name[1] != 'o' || d.name[2] != '\\0') return 2;\n"
            "    if (d.name[7] != '\\0') return 3;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 8: 2-D char array rows */
    {
        const char *src =
            "int main(void) {\n"
            "    char m[2][4] = { \"ab\", \"cd\" };\n"
            "    if (m[0][0] != 'a' || m[0][1] != 'b' || m[0][2] != '\\0' || m[0][3] != '\\0') return 1;\n"
            "    if (m[1][0] != 'c' || m[1][1] != 'd' || m[1][2] != '\\0' || m[1][3] != '\\0') return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 9: unsized 2-D char array infers the outer dimension */
    {
        const char *src =
            "int main(void) {\n"
            "    char u[][4] = { \"xy\", \"wz\" };\n"
            "    if (u[0][0] != 'x' || u[0][1] != 'y' || u[0][3] != '\\0') return 1;\n"
            "    if (u[1][0] != 'w' || u[1][1] != 'z' || u[1][3] != '\\0') return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 10: name reuse in sibling blocks (shadowing) must not corrupt
     * the earlier aggregate initializer's symbol resolution. */
    {
        const char *src =
            "struct P { int id; char name[8]; };\n"
            "int main(void) {\n"
            "    { struct P p = { 42, \"hi\" };\n"
            "      if (p.id != 42 || p.name[0] != 'h' || p.name[1] != 'i') return 1;\n"
            "    }\n"
            "    { const char *p = \"ptr\";\n"
            "      if (p[0] != 'p' || p[2] != 'r') return 2;\n"
            "    }\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 11: pointer-to-string control (still yields an address) */
    {
        const char *src =
            "int main(void) {\n"
            "    const char *p = \"ptr\";\n"
            "    if (p == 0) return 1;\n"
            "    if (p[0] != 'p' || p[2] != 'r') return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 12: file-scope struct with a char-array member */
    {
        const char *src =
            "struct S { int id; char name[8]; };\n"
            "struct S g = { 7, \"glob\" };\n"
            "int main(void) {\n"
            "    if (g.id != 7) return 1;\n"
            "    if (g.name[0] != 'g' || g.name[3] != 'b' || g.name[4] != '\\0') return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 13: file-scope unsigned char[] from a string literal */
    {
        const char *src =
            "unsigned char gu[] = \"glob\";\n"
            "int main(void) {\n"
            "    if (gu[0] != 'g' || gu[1] != 'l' || gu[3] != 'b' || gu[4] != '\\0') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 14: file-scope sized char[] truncated, zero padded */
    {
        const char *src =
            "char gs[5] = \"abcdef\";\n"
            "int main(void) {\n"
            "    if (gs[0] != 'a' || gs[1] != 'b' || gs[2] != 'c' || gs[3] != 'd' || gs[4] != 'e') return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    printf("All string-literal initializer tests passed.\n");
    return 0;
}
