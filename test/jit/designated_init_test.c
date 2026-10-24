#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

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
    printf("Running Designated / Brace Initializer JIT tests...\n");

    /* Test 1: Struct designated initializer */
    {
        const char *src =
            "struct Point { int x; int y; };\n"
            "int main(void) {\n"
            "    struct Point p = { .y = 30, .x = 12 };\n"
            "    return p.x + p.y;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 2: Struct positional brace initializer */
    {
        const char *src =
            "struct Vector { int a; int b; int c; };\n"
            "int main(void) {\n"
            "    struct Vector v = { 10, 20, 12 };\n"
            "    return v.a + v.b + v.c;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 3: Array positional brace initializer */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[4] = { 5, 10, 15, 12 };\n"
            "    return arr[0] + arr[1] + arr[2] + arr[3];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 4: Array designated initializer with indices */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[5] = { [1] = 20, [3] = 22 };\n"
            "    return arr[1] + arr[3];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 5: Zero-initialization of omitted gaps in arrays and structs */
    {
        const char *src =
            "struct Gap { int a; int b; int c; };\n"
            "int main(void) {\n"
            "    struct Gap g = { .b = 42 };\n"
            "    int arr[6] = { [2] = 40, [5] = 2 };\n"
            "    if (g.a != 0 || g.c != 0) return 1;\n"
            "    if (arr[0] != 0 || arr[1] != 0 || arr[3] != 0 || arr[4] != 0) return 2;\n"
            "    return g.b + arr[0] + arr[1];\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 6: Array designated initializer out-of-bounds rejected */
    {
        const char *src =
            "int main(void) {\n"
            "    int arr[3] = { [5] = 10 };\n"
            "    return 0;\n"
            "}\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        assert(comp->had_error != 0);
        assert(strstr(comp->error_msg, "exceeds array bound") != NULL);
               zero_cc_compiler_free(comp);
    }

    /* Test 7: VLA with initializer rejected */
    {
        const char *src =
            "int main(void) {\n"
            "    int n = 5;\n"
            "    int vla[n] = { 1, 2, 3 };\n"
            "    return 0;\n"
            "}\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        assert(comp->had_error != 0);
        assert(strstr(comp->error_msg, "cannot be initialized") != NULL);
               zero_cc_compiler_free(comp);
    }


    /* Test 8: nested sub-aggregate designator (Sprint 44 regression) */
    {
        const char *src =
            "struct P { int a; int b; }\n"
            "struct O { struct P p; int v; }\n"
            "int main(void) {\n"
            "    struct O o = { .p = { .b = 2 }, .v = 9 };\n"
            "    if (o.p.a != 0 || o.p.b != 2 || o.v != 9) return 1;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 9: .member[i] element designator, local and global */
    {
        const char *src =
            "struct O { int v; int arr[5]; };\n"
            "static struct O g = { .arr[1] = 5, .v = 7 };\n"
            "int main(void) {\n"
            "    struct O o = { .arr[2] = 5, .v = 1 };\n"
            "    if (o.arr[2] != 5 || o.arr[0] != 0 || o.v != 1) return 1;\n"
            "    if (g.arr[1] != 5 || g.arr[2] != 0 || g.v != 7) return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 10: designator chains, incl. element-then-member (.a[i].b) */
    {
        const char *src =
            "struct P { int a; int b; };\n"
            "struct O2 { struct P p; int v; int arr[4]; };\n"
            "struct Deep { int top; struct P mid; struct P inner[2]; };\n"
            "static struct Deep d2 = { .mid.b = 6, .inner[1].a = 7 };\n"
            "int main(void) {\n"
            "    struct O2 o = { .p.a = 3, .p.b = 4, .arr[0] = 1 };\n"
            "    if (o.p.a != 3 || o.p.b != 4 || o.arr[0] != 1) return 1;\n"
            "    if (d2.mid.b != 6 || d2.mid.a != 0) return 2;\n"
            "    if (d2.inner[1].a != 7 || d2.inner[1].b != 0) return 3;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 11: [i..j] ranges (scalar and sub-list values) */
    {
        const char *src =
            "struct O { int v; int arr[4]; };\n"
            "static int arr8[8] = { [2] = 7, [5 ... 6] = 3, 1 };\n"
            "int main(void) {\n"
            "    struct O a = { .arr[1 ... 2] = 9, .v = 1 };\n"
            "    struct O b = { .arr[0 ... 3] = 8, .v = 2 };\n"
            "    if (a.arr[0] != 0 || a.arr[1] != 9 || a.arr[2] != 9 || a.arr[3] != 0) return 1;\n"
            "    if (b.arr[0] != 8 || b.arr[3] != 8 || b.v != 2) return 2;\n"
            "    if (arr8[2] != 7 || arr8[5] != 3 || arr8[6] != 3) return 3;\n"
            "    /* C99 6.7.9: positional after designations continues past them */\n"
            "    if (arr8[7] != 1) return 4;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 12: elided braces for arrays of structs, local and global */
    {
        const char *src =
            "struct P { int a; int b; };\n"
            "static struct P ptab[2] = { 1, 2, 3, 4 };\n"
            "int main(void) {\n"
            "    struct P pl[2] = { 10, 20, 30, 40 };\n"
            "    if (pl[0].a != 10 || pl[0].b != 20 || pl[1].a != 30 || pl[1].b != 40) return 1;\n"
            "    if (ptab[0].a != 1 || ptab[0].b != 2 || ptab[1].a != 3 || ptab[1].b != 4) return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 13: transparent grouping braces and mixed positional/designated */
    {
        const char *src =
            "int main(void) {\n"
            "    int ia[3] = { {1}, 2, {3} };\n"
            "    int iq[4] = { 1, [2] = 7, 2 };\n"
            "    if (ia[0] != 1 || ia[1] != 2 || ia[2] != 3) return 1;\n"
            "    if (iq[0] != 1 || iq[1] != 0 || iq[2] != 7 || iq[3] != 2) return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 14: 2-D arrays (grouped rows, [r][c] designators, char rows) */
    {
        const char *src =
            "int main(void) {\n"
            "    int row[2][3] = { {1, 2, 3}, {4, 5, 6} };\n"
            "    int row2[2][3] = { [1][2] = 99, [0][0] = 7 };\n"
            "    char m[2][4] = { \"ab\", \"cd\" };\n"
            "    char u[][4] = { \"xy\", \"wz\" };\n"
            "    if (row[0][2] != 3 || row[1][0] != 4) return 1;\n"
            "    if (row2[0][0] != 7 || row2[1][2] != 99 || row2[0][1] != 0) return 2;\n"
            "    if (m[0][0] != 'a' || m[0][2] != '\\0' || m[1][1] != 'd') return 3;\n"
            "    if (u[1][0] != 'w' || u[1][3] != '\\0') return 4;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 15: compound literals with nested sub-aggregate designators */
    {
        const char *src =
            "struct P { int a; int b; };\n"
            "struct O { struct P p; int v; };\n"
            "int main(void) {\n"
            "    struct O *ocl = &(struct O){ .p = { .b = 5 }, .v = 11 };\n"
            "    struct P *pcl = &(struct P){ .a = 3, .b = 4 };\n"
            "    if (ocl->p.a != 0 || ocl->p.b != 5 || ocl->v != 11) return 1;\n"
            "    if (pcl->a != 3 || pcl->b != 4) return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    /* Test 16: global struct with nested designators (static data path) */
    {
        const char *src =
            "struct P { int a; int b; };\n"
            "struct O { struct P p; int v; int arr[4]; };\n"
            "static struct O s1 = { .p = { .b = 2 }, .v = 9 };\n"
            "static struct O s2 = { .p = { .b = 2 }, .arr[1] = 5, .v = 7 };\n"
            "int main(void) {\n"
            "    if (s1.p.a != 0 || s1.p.b != 2 || s1.v != 9) return 1;\n"
            "    if (s2.p.b != 2 || s2.arr[1] != 5 || s2.arr[2] != 0 || s2.v != 7) return 2;\n"
            "    return 0;\n"
            "}\n";
        assert(run_snippet(src) == 0);
    }

    printf("All Designated / Brace Initializer JIT tests passed!\n");
    return 0;
}
