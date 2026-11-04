/* Regression tests: GNU variably-modified struct member diagnostics.
 *
 * 1. Sibling-scope struct tag redefinition with a DIFFERENT layout must
 *    be rejected cleanly (the global struct registry cannot represent
 *    two block-scope definitions of the same tag; GCC can - c-torture
 *    execute/20070919-1).
 * 2. Sibling-scope redefinition with the SAME layout is legal C and must
 *    compile.
 * 3. A complete definition followed by a bare forward declaration
 *    (`struct S;`) must compile (incomplete->complete and the reverse
 *    must not trip the layout check).
 * 4. A forward declaration completed later (typedef struct X X; ...
 *    struct X { int y; };) must compile. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc.h"

typedef enum { WANT_ERROR, WANT_OK } ExpectKind;

static int run_case(const char *src, ExpectKind expect, const char *expect_substr) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    if (!comp) { perror("calloc"); return 1; }

    char *prep = zero_cc_preprocess(src, NULL);
    if (!prep) { fprintf(stderr, "Preprocess failed\n"); return 1; }

    zero_cc_tokenize_c99(prep, comp);
    free(prep);

    zero_cc_compile_c99(comp);

    int rc = 0;
    if (expect == WANT_ERROR && !comp->had_error) {
        fprintf(stderr, "FAIL: expected error for: %s\n", src);
        rc = 1;
    } else if (expect == WANT_OK && comp->had_error) {
        fprintf(stderr, "FAIL: unexpected error '%s' for: %s\n",
                comp->error_msg, src);
        rc = 1;
    } else if (expect_substr && comp->had_error &&
               strstr(comp->error_msg, expect_substr) == NULL) {
        fprintf(stderr, "FAIL: error '%s' missing expected text '%s'\n",
                comp->error_msg, expect_substr);
        rc = 1;
    }

    zero_cc_compiler_free(comp);
    return rc;
}

int main(void) {
    int fails = 0;

    /* 1. Same tag, different layouts in sibling scopes (20070919-1). */
    fails += run_case(
        "int f1 (int y) { struct S { char w[y]; } a; return a.w[0]; }\n"
        "int f2 (void) { struct S { char w[8]; } b; return b.w[0]; }\n"
        "int main (void) { return f2(); }\n",
        WANT_ERROR, "redefinition of 'struct S'");

    /* 2. Same tag, same layout in sibling scopes - legal. */
    fails += run_case(
        "int f1 (int y) { struct S { int a; } s; s.a = y; return s.a; }\n"
        "int f2 (int y) { struct S { int a; } s; s.a = y; return s.a; }\n"
        "int main (void) { return f1(1) + f2(2); }\n",
        WANT_OK, NULL);

    /* 3. Complete definition then bare forward declaration. */
    fails += run_case(
        "struct T { int a; };\n"
        "struct T;\n"
        "int main (void) { struct T t; t.a = 4; return t.a; }\n",
        WANT_OK, NULL);

    /* 4. Forward declaration completed later. */
    fails += run_case(
        "typedef struct X X;\n"
        "struct X { int y; };\n"
        "int main (void) { X x; x.y = 7; return x.y; }\n",
        WANT_OK, NULL);

    /* 5. Identical block-scope redefinitions with VLA members (same
     * static placeholder layout) are fine. */
    fails += run_case(
        "int f1 (int y) { struct V { int a; char b[y]; } v; v.a = 1; return v.a; }\n"
        "int f2 (int y) { struct V { int a; char b[y]; } v; v.a = 2; return v.a; }\n"
        "int main (int argc, char **argv) { return f1(argc) + f2(argc); }\n",
        WANT_OK, NULL);

    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("vla_struct_diag_test: all cases passed\n");
    return 0;
}
