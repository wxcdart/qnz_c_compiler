#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include "zero_cc.h"

// Helper to compile a source string and capture diagnostics into a temporary file.
static void run_test(const char *src, bool expect_error, const char *expected_msg) {
    // Redirect stderr to a temporary file to capture diagnostics.
    FILE *err_file = tmpfile();
    assert(err_file != NULL);
    int err_fd = fileno(err_file);
    fflush(stderr);
    int saved_err = dup(fileno(stderr));
    dup2(err_fd, fileno(stderr));

    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    assert(comp != NULL);
    char *prep = zero_cc_preprocess(src, NULL);
    assert(prep != NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);

    // Restore original stderr.
    fflush(stderr);
    dup2(saved_err, fileno(stderr));
    close(saved_err);

    // Read captured diagnostics.
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

    if (expect_error) {
        assert(comp->had_error && "Expected an error but none occurred");
        if (expected_msg != NULL) {
            assert(diag_buf != NULL && "No diagnostics output captured");
            assert(strstr(diag_buf, expected_msg) && "Diagnostic message not found");
        }
    } else {
        assert(!comp->had_error && "Expected no errors but some were reported");
    }

    // Cleanup
    zero_cc_compiler_free(comp);
    if (diag_buf) free(diag_buf);
}

int main(void) {
    // 1. Break outside any loop – semantic error, expect "break" message.
    run_test("int main(void) { break; }", true, "break");
    // 2. Use undefined identifier – expect "undefined".
    run_test("int main(void) { int x = undefined_var; }", true, "undefined");
    // 3. Duplicate variable definition – expect "redefinition".
    run_test("int main(void) { int x = 1; int x = 2; }", true, "redefinition");
    // 4. Type mismatch in return – returning a float from int is allowed in C, so expect no error.
    run_test("int main(void) { return 1.0; }", false, NULL);
    // 5. Correct program – no error.
    run_test("int main(void) { return 0; }", false, NULL);
    return 0;
}
