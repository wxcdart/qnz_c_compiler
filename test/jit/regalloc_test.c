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
        free(comp->tokens);
        free(comp);
        return -1;
    }
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}

int main(void) {
    printf("Running register allocation tests...\n");

    /* Test 1: value live across a call must survive (callee-save preservation) */
    {
        const char *src =
            "int heavy(int a, int b, int c, int d) {\n"
            "    int t1 = a + b; int t2 = c + d;\n"
            "    int t4 = t1 * t2;\n"
            "    return t4 + a;\n"
            "}\n"
            "int main(void) {\n"
            "    int saved = 42;\n"
            "    int r = heavy(1, 2, 3, 4);\n"
            "    return saved + r;\n"
            "}\n";
        assert(run_snippet(src) == 64);
    }

    /* Test 2: many simultaneously live values */
    {
        const char *src =
            "int main(void) {\n"
            "    int x = 0;\n"
            "    int a = x + 1; int b = x + 2; int c = x + 3; int d = x + 4;\n"
            "    int e = x + 5; int f = x + 6; int g = x + 7; int h = x + 8;\n"
            "    int i = x + 9; int j = x + 10;\n"
            "    return a + b + c + d + e + f + g + h + i + j;\n"
            "}\n";
        assert(run_snippet(src) == 55);
    }

    /* Test 3: recursion */
    {
        const char *src =
            "int fib(int n) {\n"
            "    if (n <= 1) return n;\n"
            "    int a = fib(n - 1);\n"
            "    int b = fib(n - 2);\n"
            "    return a + b;\n"
            "}\n"
            "int main(void) { return fib(10); }\n";
        assert(run_snippet(src) == 55);
    }

    /* Test 4: five and six register arguments (r8, r9) */
    {
        const char *src =
            "int f(int a, int b, int c, int d, int e, int g) {\n"
            "    return a + b * 2 + c * 3 + d * 4 + e * 5 + g * 6;\n"
            "}\n"
            "int main(void) { return f(1, 2, 3, 4, 5, 6); }\n";
        /* 1 + 4 + 9 + 16 + 25 + 36 = 91 */
        assert(run_snippet(src) == 91);
    }

    /* Test 5: seventh and eighth arguments are passed on the stack */
    {
        const char *src =
            "int f(int a, int b, int c, int d, int e, int g, int h, int k) {\n"
            "    return a + b * 2 + c * 3 + d * 4 + e * 5 + g * 6 + h * 7 + k * 8;\n"
            "}\n"
            "int main(void) { return f(1, 2, 3, 4, 5, 6, 7, 8); }\n";
        /* 91 + 49 + 64 = 204 */
        assert(run_snippet(src) == 204);
    }

    /* Test 6: seven args (odd stack count -> alignment padding) with live value across call */
    {
        const char *src =
            "int f(int a, int b, int c, int d, int e, int g, int h) {\n"
            "    return a + b + c + d + e + g + h;\n"
            "}\n"
            "int main(void) {\n"
            "    int keep = 100;\n"
            "    int r = f(1, 2, 3, 4, 5, 6, 7);\n"
            "    return keep + r;\n"
            "}\n";
        assert(run_snippet(src) == 128);
    }

    /* Test 7: nested function call arguments clobbering RAX */
    {
        const char *src =
            "int add(int a, int b) { return a + b; }\n"
            "int sub(int a, int b) { return a - b; }\n"
            "int combine(int a, int b, int c) { return a * 100 + b * 10 + c; }\n"
            "int main(void) {\n"
            "    /* combine(add(1, 2), sub(10, 5), add(3, 4)) -> combine(3, 5, 7) = 357 */\n"
            "    return combine(add(1, 2), sub(10, 5), add(3, 4));\n"
            "}\n";
        assert(run_snippet(src) == 357);
    }

    /* Test 8: Phase 2 - multiple live values spanning an intervening function call */
    {
        const char *src =
            "int clobber_caller_regs(int x) {\n"
            "    /* Perform arithmetic across caller-saved regs (rdi, rsi, rdx, rcx, r8, r9, rax) */\n"
            "    int a = x * 2; int b = x * 3; int c = x * 4;\n"
            "    return a + b + c;\n"
            "}\n"
            "int main(void) {\n"
            "    int v1 = 10;\n"
            "    int v2 = 20;\n"
            "    int v3 = 30;\n"
            "    int mid = clobber_caller_regs(5);\n"
            "    /* v1, v2, v3 span the call to clobber_caller_regs and must survive intact */\n"
            "    return v1 + v2 + v3 + mid;\n"
            "}\n";
        /* 10 + 20 + 30 + (10 + 15 + 20) = 60 + 45 = 105 */
        assert(run_snippet(src) == 105);
    }

    /* Test 9: Phase 3 - Extreme register pressure forcing true stack spilling.
     * With 16+ live variables held simultaneously, all 12 allocator registers are
     * exhausted, forcing multiple temporaries to spill to the stack frame. */
    {
        const char *src =
            "int main(void) {\n"
            "    int a = 1;  int b = 2;  int c = 3;  int d = 4;\n"
            "    int e = 5;  int f = 6;  int g = 7;  int h = 8;\n"
            "    int i = 9;  int j = 10; int k = 11; int l = 12;\n"
            "    int m = 13; int n = 14; int o = 15; int p = 16;\n"
            "    int q = 17; int r = 18;\n"
            "    /* All 18 variables live simultaneously at this point */\n"
            "    int sum = a + b + c + d + e + f + g + h + i + j + k + l + m + n + o + p + q + r;\n"
            "    return sum;\n"
            "}\n";
        /* 18 * 19 / 2 = 171 */
        assert(run_snippet(src) == 171);
    }

    /* Test 10: Phase 3 - Spilled variables spanning a function call with two-address ALU operations */
    {
        const char *src =
            "int helper(int x) { return x + 1; }\n"
            "int main(void) {\n"
            "    int a = 1;  int b = 2;  int c = 3;  int d = 4;\n"
            "    int e = 5;  int f = 6;  int g = 7;  int h = 8;\n"
            "    int i = 9;  int j = 10; int k = 11; int l = 12;\n"
            "    int m = 13; int n = 14;\n"
            "    int mid = helper(100);\n"
            "    int diff = (a + b + c + d + e + f + g) - (h + i + j + k + l + m + n) + mid;\n"
            "    return diff;\n"
            "}\n";
        /* (1..7 sum = 28) - (8..14 sum = 77) + 101 = 28 - 77 + 101 = 52 */
        assert(run_snippet(src) == 52);
    }

    /* Test 11: Spilled intervals spanning calls in both JIT and AOT assembly emission */
    {
        const char *src =
            "int clobber(int x, int y, int z, int w, int u, int v) {\n"
            "    return x + y + z + w + u + v;\n"
            "}\n"
            "int main(void) {\n"
            "    int x = 1;\n"
            "    int a = x + 1; int b = x + 2; int c = x + 3; int d = x + 4;\n"
            "    int e = x + 5; int f = x + 6; int g = x + 7;\n"
            "    int res = (a * 2) + (b * 3) + (c * 4) + (d * 5) + (e * 6) + (f * 7) + (g * 8)\n"
            "              + clobber(a, b, c, d, e, f);\n"
            "    return res - 100;\n"
            "}\n";
        /* a=2,b=3,c=4,d=5,e=6,f=7,g=8
         * 4 + 9 + 16 + 25 + 36 + 49 + 64 = 203
         * clobber(2,3,4,5,6,7) = 27
         * res - 100 = 230 - 100 = 130 */
        assert(run_snippet(src) == 130);

        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        assert(comp != NULL);
        char *asm_text = NULL;
        size_t asm_len = 0;
        comp->emit_asm = 1;
        comp->asm_out = open_memstream(&asm_text, &asm_len);
        assert(comp->asm_out != NULL);
        char *prep = zero_cc_preprocess(src, NULL);
        assert(prep != NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        fclose(comp->asm_out);
        comp->asm_out = NULL;
        assert(asm_text != NULL && asm_len > 0 && !comp->had_error);
        free(asm_text);
        zero_cc_buf_free(&comp->text_section);
        zero_cc_buf_free(&comp->rodata_section);
        zero_cc_buf_free(&comp->data_section);
        free(comp->tokens);
        free(comp);
    }

    printf("All register allocation tests passed.\n");
    return 0;
}

