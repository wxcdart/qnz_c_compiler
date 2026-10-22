#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "zero_cc.h"

static int run_snippet(const char *src) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    assert(!comp->had_error && "Compilation failed unexpectedly");
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_compiler_free(comp);
    return ret;
}

static void run_error_test(const char *src, const char *expected_msg) {
    FILE *err_file = tmpfile();
    assert(err_file != NULL);
    int err_fd = fileno(err_file);
    fflush(stderr);
    int saved_err = dup(fileno(stderr));
    dup2(err_fd, fileno(stderr));

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);

    fflush(stderr);
    dup2(saved_err, fileno(stderr));
    close(saved_err);

    fseek(err_file, 0, SEEK_SET);
    char *diag_buf = NULL;
    size_t diag_len = 0;
    {
        fseek(err_file, 0, SEEK_END);
        diag_len = ftell(err_file);
        fseek(err_file, 0, SEEK_SET);
        if (diag_len > 0) {
            diag_buf = malloc(diag_len + 1);
            fread(diag_buf, 1, diag_len, err_file);
            diag_buf[diag_len] = '\0';
        }
    }
    fclose(err_file);

    assert(comp->had_error && "Expected error but compilation succeeded");
    if (expected_msg) {
        bool found = false;
        if (diag_buf && strstr(diag_buf, expected_msg)) {
            found = true;
        } else if (comp->error_msg[0] && strstr(comp->error_msg, expected_msg)) {
            found = true;
        }
        assert(found && "Expected diagnostic message not found in output");
    }

    zero_cc_compiler_free(comp);
    if (diag_buf) free(diag_buf);
}

int main(void) {
    printf("Running const/volatile semantic enforcement test suite...\n");

    /* Test 1: Read const variable */
    {
        const char *src =
            "int main(void) {\n"
            "    const int x = 42;\n"
            "    return x;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (const int read): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 2: Read pointer to const int */
    {
        const char *src =
            "int main(void) {\n"
            "    const int x = 25;\n"
            "    const int *p = &x;\n"
            "    return *p + 5;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 2 (const int * read): %d (expected 30)\n", res);
        assert(res == 30);
    }

    /* Test 3: Modify through const pointer to mutable int */
    {
        const char *src =
            "int main(void) {\n"
            "    int x = 10;\n"
            "    int * const p = &x;\n"
            "    *p = 55;\n"
            "    return *p;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 3 (int * const modify target): %d (expected 55)\n", res);
        assert(res == 55);
    }

    /* Test 4: Volatile variable write and read */
    {
        const char *src =
            "int main(void) {\n"
            "    volatile int v = 100;\n"
            "    v = 200;\n"
            "    v += 50;\n"
            "    return v;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 4 (volatile int ops): %d (expected 250)\n", res);
        assert(res == 250);
    }

    /* Test 5: int volatile postfix/prefix */
    {
        const char *src =
            "int main(void) {\n"
            "    int volatile v = 12;\n"
            "    v++;\n"
            "    return v;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 5 (int volatile inc): %d (expected 13)\n", res);
        assert(res == 13);
    }

    /* Test 6: Pointer to volatile int */
    {
        const char *src =
            "int main(void) {\n"
            "    volatile int v = 7;\n"
            "    volatile int *pv = &v;\n"
            "    *pv = 42;\n"
            "    return *pv;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 6 (volatile int *): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 7: Struct with const member, modify mutable member */
    {
        const char *src =
            "struct Config {\n"
            "    const int id;\n"
            "    int val;\n"
            "};\n"
            "int main(void) {\n"
            "    struct Config c;\n"
            "    c.val = 50;\n"
            "    return c.val;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 7 (struct const member): %d (expected 50)\n", res);
        assert(res == 50);
    }

    /* Test 8: Const struct read */
    {
        const char *src =
            "struct Point {\n"
            "    int x;\n"
            "    int y;\n"
            "};\n"
            "int main(void) {\n"
            "    const struct Point p;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 8 (const struct read): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 9: Const volatile int */
    {
        const char *src =
            "int main(void) {\n"
            "    const volatile int cv = 99;\n"
            "    return cv;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 9 (const volatile int): %d (expected 99)\n", res);
        assert(res == 99);
    }

    /* --- Semantic enforcement error tests --- */

    /* Test 10: Error on assignment to const variable */
    {
        const char *src = "int main(void) { const int x = 1; x = 2; return x; }";
        run_error_test(src, "const");
        printf("Test 10 (reject x = 2 on const x): PASS\n");
    }

    /* Test 11: Error on post-increment of const variable */
    {
        const char *src = "int main(void) { const int x = 1; x++; return x; }";
        run_error_test(src, "const");
        printf("Test 11 (reject x++ on const x): PASS\n");
    }

    /* Test 12: Error on pre-decrement of const variable */
    {
        const char *src = "int main(void) { const int x = 1; --x; return x; }";
        run_error_test(src, "const");
        printf("Test 12 (reject --x on const x): PASS\n");
    }

    /* Test 13: Error on compound assignment to const variable */
    {
        const char *src = "int main(void) { const int x = 1; x += 5; return x; }";
        run_error_test(src, "const");
        printf("Test 13 (reject x += 5 on const x): PASS\n");
    }

    /* Test 14: Error on assignment through pointer to const */
    {
        const char *src = "int main(void) { const int x = 1; const int *p = &x; *p = 2; return 0; }";
        run_error_test(src, "const");
        printf("Test 14 (reject *p = 2 on const int *p): PASS\n");
    }

    /* Test 15: Error on assignment to const pointer */
    {
        const char *src = "int main(void) { int x = 1; int y = 2; int * const p = &x; p = &y; return 0; }";
        run_error_test(src, "const");
        printf("Test 15 (reject p = &y on int * const p): PASS\n");
    }

    /* Test 16: Error on assignment to const struct member */
    {
        const char *src =
            "struct S { const int a; int b; };\n"
            "int main(void) { struct S s; s.a = 5; return 0; }";
        run_error_test(src, "const");
        printf("Test 16 (reject s.a = 5 on const member): PASS\n");
    }

    /* Test 17: Error on assignment to member of const struct */
    {
        const char *src =
            "struct S { int a; int b; };\n"
            "int main(void) { const struct S s; s.a = 5; return 0; }";
        run_error_test(src, "const");
        printf("Test 17 (reject s.a = 5 on const struct): PASS\n");
    }

    /* Test 18: Error on assigning const int* to int* (discards const qualifier) */
    {
        const char *src =
            "int main(void) { const int x = 1; const int *p = &x; int *q = p; return 0; }";
        run_error_test(src, "incompatible");
        printf("Test 18 (reject int *q = p from const int *): PASS\n");
    }

    /* Test 19: Error on assigning volatile int* to int* (discards volatile qualifier) */
    {
        const char *src =
            "int main(void) { volatile int x = 1; volatile int *p = &x; int *q = p; return 0; }";
        run_error_test(src, "incompatible");
        printf("Test 19 (reject int *q = p from volatile int *): PASS\n");
    }

    printf("All const/volatile tests passed successfully!\n");
    return 0;
}
