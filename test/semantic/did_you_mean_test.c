/* Regression tests: clang/gcc-style "did you mean" suggestions on
 * undeclared identifiers and unknown struct members.
 *
 * The suggestion fires when a candidate is within Levenshtein distance
 * <= 2 (and <= max(1, len/2) for short names) and is the unique best
 * match; ambiguous (tied) or distant names must NOT be suggested —
 * a wrong suggestion is worse than none. */
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
        fprintf(stderr, "FAIL: unexpected error '%s' for: %s\n", comp->error_msg, src);
        rc = 1;
    } else if (expect_substr && strstr(comp->error_msg, expect_substr) == NULL) {
        fprintf(stderr, "FAIL: error '%s' missing expected text '%s'\n",
                comp->error_msg, expect_substr);
        rc = 1;
    }

    zero_cc_compiler_free(comp);
    return rc;
}

/* Like run_case, but asserts the message does NOT contain the substring
 * (used to verify that tied/distant names are not suggested). */
static int run_case_absent(const char *src, ExpectKind expect, const char *absent_substr) {
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
        fprintf(stderr, "FAIL: unexpected error '%s' for: %s\n", comp->error_msg, src);
        rc = 1;
    } else if (comp->had_error && strstr(comp->error_msg, absent_substr) != NULL) {
        fprintf(stderr, "FAIL: error '%s' must not contain '%s'\n",
                comp->error_msg, absent_substr);
        rc = 1;
    }

    zero_cc_compiler_free(comp);
    return rc;
}

int main(void) {
    int fails = 0;

    /* 1. Transposition typo of a variable name */
    fails += run_case(
        "int counter = 3;\nint main(void) { return countr; }\n",
        WANT_ERROR, "did you mean 'counter'");

    /* 2. Typo of a function name (functions are scope symbols too) */
    fails += run_case(
        "int add(int a, int b) { return a + b; }\n"
        "int main(void) { return addd(1, 2); }\n",
        WANT_ERROR, "did you mean 'add'");

    /* 3. Unknown struct member, close field exists */
    fails += run_case(
        "struct P { int x; int y; };\n"
        "int main(void) { struct P p; p.y2 = 2; return p.x; }\n",
        WANT_ERROR, "did you mean 'y'");

    /* 4. Distant name: no suggestion */
    fails += run_case_absent(
        "int counter = 3;\nint main(void) { return zzz; }\n",
        WANT_ERROR, "did you mean");

    /* 5. Tied candidates (both 'adc' and 'xbc' at distance 1 from 'abc'):
     * ambiguous — no suggestion */
    fails += run_case_absent(
        "int adc = 1; int xbc = 2;\nint main(void) { return abc; }\n",
        WANT_ERROR, "did you mean");

    /* 6. Valid program: no error at all */
    fails += run_case(
        "int counter = 3;\nint main(void) { return counter; }\n",
        WANT_OK, NULL);

    if (fails) {
        fprintf(stderr, "%d did-you-mean case(s) failed\n", fails);
        return 1;
    }
    printf("PASS: did-you-mean suggestions (6/6 cases)\n");
    return 0;
}
