#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "zero_cc.h"

static int run_snippet(const char *src) {
    ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
    char *prep = zero_cc_preprocess(src, NULL);
    zero_cc_tokenize_c99(prep, comp);
    free(prep);
    zero_cc_compile_c99(comp);
    int ret = zero_cc_execute_jit(comp, NULL);
    zero_cc_buf_free(&comp->text_section);
    zero_cc_buf_free(&comp->rodata_section);
    zero_cc_buf_free(&comp->data_section);
    free(comp->tokens);
    free(comp);
    return ret;
}

int main(void) {
    printf("Running Atomic test suite...\n");

    /* Test 1: _Atomic int declaration, increment (postfix & prefix) */
    {
        const char *src =
            "int main(void) {\n"
            "    _Atomic int counter = 10;\n"
            "    int old1 = counter++;\n"
            "    int new1 = ++counter;\n"
            "    if (old1 != 10) return 1;\n"
            "    if (new1 != 12) return 2;\n"
            "    if (counter != 12) return 3;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 1 (_Atomic int inc): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 2: _Atomic int decrement and compound assign (+=, -=) */
    {
        const char *src =
            "int main(void) {\n"
            "    _Atomic int counter = 20;\n"
            "    int old = counter--;\n"
            "    int pref = --counter;\n"
            "    if (old != 20) return 1;\n"
            "    if (pref != 18) return 2;\n"
            "    counter += 10;\n"
            "    if (counter != 28) return 3;\n"
            "    counter -= 8;\n"
            "    if (counter != 20) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 2 (_Atomic int dec & compound): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 3: _Atomic(int) syntax and simple assignment (xchg) */
    {
        const char *src =
            "int main(void) {\n"
            "    _Atomic(int) flag = 0;\n"
            "    flag = 99;\n"
            "    if (flag != 99) return 1;\n"
            "    flag = 123;\n"
            "    if (flag != 123) return 2;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 3 (_Atomic(T) & assign): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 4: __sync_fetch_and_add and __sync_add_and_fetch */
    {
        const char *src =
            "int main(void) {\n"
            "    int val = 100;\n"
            "    int old_v = __sync_fetch_and_add(&val, 25);\n"
            "    if (old_v != 100) return 1;\n"
            "    if (val != 125) return 2;\n"
            "    int new_v = __sync_add_and_fetch(&val, 15);\n"
            "    if (new_v != 140) return 3;\n"
            "    if (val != 140) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 4 (__sync_fetch_and_add / __sync_add_and_fetch): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 5: __sync_fetch_and_sub and __sync_sub_and_fetch */
    {
        const char *src =
            "int main(void) {\n"
            "    int val = 50;\n"
            "    int old_v = __sync_fetch_and_sub(&val, 20);\n"
            "    if (old_v != 50) return 1;\n"
            "    if (val != 30) return 2;\n"
            "    int new_v = __sync_sub_and_fetch(&val, 10);\n"
            "    if (new_v != 20) return 3;\n"
            "    if (val != 20) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 5 (__sync_fetch_and_sub / __sync_sub_and_fetch): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 6: __sync_val_compare_and_swap */
    {
        const char *src =
            "int main(void) {\n"
            "    int val = 55;\n"
            "    /* Mismatch: expected 10, actual 55 -> no change, returns 55 */\n"
            "    int r1 = __sync_val_compare_and_swap(&val, 10, 99);\n"
            "    if (r1 != 55) return 1;\n"
            "    if (val != 55) return 2;\n"
            "    /* Match: expected 55, actual 55 -> updates to 99, returns 55 */\n"
            "    int r2 = __sync_val_compare_and_swap(&val, 55, 99);\n"
            "    if (r2 != 55) return 3;\n"
            "    if (val != 99) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 6 (__sync_val_compare_and_swap): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 7: __sync_bool_compare_and_swap */
    {
        const char *src =
            "int main(void) {\n"
            "    int val = 77;\n"
            "    /* Mismatch */\n"
            "    int ok1 = __sync_bool_compare_and_swap(&val, 10, 88);\n"
            "    if (ok1 != 0) return 1;\n"
            "    if (val != 77) return 2;\n"
            "    /* Match */\n"
            "    int ok2 = __sync_bool_compare_and_swap(&val, 77, 88);\n"
            "    if (ok2 != 1) return 3;\n"
            "    if (val != 88) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 7 (__sync_bool_compare_and_swap): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 8: __sync_lock_test_and_set, __sync_lock_release, __sync_synchronize */
    {
        const char *src =
            "int main(void) {\n"
            "    int lock = 0;\n"
            "    int prev1 = __sync_lock_test_and_set(&lock, 1);\n"
            "    if (prev1 != 0) return 1;\n"
            "    if (lock != 1) return 2;\n"
            "    int prev2 = __sync_lock_test_and_set(&lock, 1);\n"
            "    if (prev2 != 1) return 3;\n"
            "    __sync_synchronize();\n"
            "    __sync_lock_release(&lock);\n"
            "    if (lock != 0) return 4;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 8 (__sync_lock_test_and_set / release / synchronize): %d (expected 42)\n", res);
        assert(res == 42);
    }

    /* Test 9: __atomic_* builtins */
    {
        const char *src =
            "int main(void) {\n"
            "    int data = 10;\n"
            "    __atomic_store_n(&data, 20, 5);\n"
            "    if (__atomic_load_n(&data, 5) != 20) return 1;\n"
            "    int old_v = __atomic_fetch_add(&data, 5, 5);\n"
            "    if (old_v != 20) return 2;\n"
            "    if (__atomic_load_n(&data, 5) != 25) return 3;\n"
            "    int new_v = __atomic_add_fetch(&data, 5, 5);\n"
            "    if (new_v != 30) return 4;\n"
            "    int prev = __atomic_exchange_n(&data, 50, 5);\n"
            "    if (prev != 30) return 5;\n"
            "    if (__atomic_load_n(&data, 5) != 50) return 6;\n"
            "    int exp = 40;\n"
            "    int ok = __atomic_compare_exchange_n(&data, &exp, 60, 0, 5, 5);\n"
            "    if (ok != 0) return 7;\n"
            "    if (exp != 50) return 8;\n"
            "    ok = __atomic_compare_exchange_n(&data, &exp, 60, 0, 5, 5);\n"
            "    if (ok != 1) return 9;\n"
            "    if (data != 60) return 10;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        printf("Test 9 (__atomic_* builtins): %d (expected 42)\n", res);
        assert(res == 42);
    }

    printf("All Atomic tests passed!\n");
    return 0;
}
