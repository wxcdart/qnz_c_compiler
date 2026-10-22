/* Test #pragma message, #pragma GCC diagnostic and push_options/pop_options */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "zero_cc.h"

#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAILED: %s (line %d)\n", msg, __LINE__); return 1; } } while (0)

/* Preprocess + tokenize; returns the compiler (caller frees). */
static ZeroCCCompiler *prep_tok(const char *src, char *captured, size_t cap) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    fflush(stderr);
    int saved = dup(2);
    FILE *tmp = tmpfile();
    dup2(fileno(tmp), 2);
    char *prep = zero_cc_preprocess(src, NULL);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    if (captured) {
        rewind(tmp);
        size_t n = fread(captured, 1, cap - 1, tmp);
        captured[n] = '\0';
    }
    fclose(tmp);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    return comp;
}

static int run_main(const char *src) {
    ZeroCCCompiler *comp = prep_tok(src, NULL, 0);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
       zero_cc_compiler_free(comp);
    return ret;
}

int main(void) {
    char out[1024];

    /* message: printed to stderr, consumed (code still compiles) */
    {
        const char *src = "#pragma message(\"hello pragma\")\nint main(void) { return 7; }\n";
        ZeroCCCompiler *c = prep_tok(src, out, sizeof(out));
        CHECK(strstr(out, "hello pragma") != NULL, "message text printed");
        zero_cc_compiler_free(c);
        CHECK(run_main(src) == 7, "program with #pragma message runs");
    }

    /* diagnostic ignored: warning disabled */
    {
        ZeroCCCompiler *c = prep_tok("#pragma GCC diagnostic ignored \"-Wunused-variable\"\nint x;\n", NULL, 0);
        CHECK(diag_is_warning_disabled(c, "unused-variable"), "ignored disables warning");
        CHECK(diag_is_warning_disabled(c, "-Wunused-variable"), "-W prefix accepted");
        CHECK(!diag_is_warning_disabled(c, "shadow"), "other warnings unaffected");
        zero_cc_compiler_free(c);
    }

    /* push / ignored / pop restores */
    {
        ZeroCCCompiler *c = prep_tok(
            "#pragma GCC diagnostic push\n"
            "#pragma GCC diagnostic ignored \"-Wshadow\"\n"
            "#pragma GCC diagnostic pop\n", NULL, 0);
        CHECK(!diag_is_warning_disabled(c, "shadow"), "pop restores state");
        zero_cc_compiler_free(c);
    }

    /* warning re-enables; error -Werror sets flag */
    {
        ZeroCCCompiler *c = prep_tok(
            "#pragma GCC diagnostic ignored \"-Wshadow\"\n"
            "#pragma GCC diagnostic warning \"-Wshadow\"\n"
            "#pragma GCC diagnostic error \"-Werror\"\n", NULL, 0);
        CHECK(!diag_is_warning_disabled(c, "shadow"), "warning re-enables");
        CHECK(c->diag_pragma_state.warning_as_error, "error -Werror sets flag");
        zero_cc_compiler_free(c);
    }

    /* state does not leak between translation units */
    {
        ZeroCCCompiler *c = prep_tok("int y;\n", NULL, 0);
        CHECK(!diag_is_warning_disabled(c, "shadow"), "fresh TU has no disabled warnings");
        CHECK(!c->diag_pragma_state.warning_as_error, "fresh TU has no -Werror");
        zero_cc_compiler_free(c);
    }

    /* push_options / pop_options and _Pragma forms are consumed */
    {
        const char *src =
            "#pragma GCC push_options\n"
            "_Pragma(\"GCC diagnostic ignored \\\"-Wfoo\\\"\")\n"
            "#pragma GCC pop_options\n"
            "int main(void) { return 3; }\n";
        CHECK(run_main(src) == 3, "push/pop_options + _Pragma diagnostic compile");
        ZeroCCCompiler *c = prep_tok(src, NULL, 0);
        CHECK(diag_is_warning_disabled(c, "foo"), "_Pragma GCC diagnostic honoured");
        zero_cc_compiler_free(c);
    }

    /* unknown pragma does not break compilation */
    CHECK(run_main("#pragma weird_vendor_thing 1 2 3\nint main(void) { return 9; }\n") == 9,
          "unknown pragma passthrough");

    printf("All pragma diagnostic tests passed!\n");
    return 0;
}
