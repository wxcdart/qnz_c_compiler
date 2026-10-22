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
    printf("Running Named Enum Type-Tag JIT tests...\n");

    /* Test 1: File-scope named enum declaration, variable, and constant values */
    {
        const char *src =
            "enum Color { RED = 5, GREEN = 10, BLUE = 20 };\n"
            "int main(void) {\n"
            "    enum Color c = GREEN;\n"
            "    if (c != 10) return 1;\n"
            "    c = BLUE;\n"
            "    if (c != 20) return 2;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 2: sizeof and alignof named enum */
    {
        const char *src =
            "enum Priority { LOW, MEDIUM, HIGH };\n"
            "int main(void) {\n"
            "    enum Priority p = HIGH;\n"
            "    if (sizeof(enum Priority) != 4) return 1;\n"
            "    if (sizeof(p) != 4) return 2;\n"
            "    if (_Alignof(enum Priority) != 4) return 3;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 3: Named enum in function parameter and return type */
    {
        const char *src =
            "enum State { STOPPED = 0, RUNNING = 1, PAUSED = 2 };\n"
            "enum State transition(enum State s) {\n"
            "    if (s == STOPPED) return RUNNING;\n"
            "    if (s == RUNNING) return PAUSED;\n"
            "    return STOPPED;\n"
            "}\n"
            "int main(void) {\n"
            "    enum State cur = STOPPED;\n"
            "    cur = transition(cur);\n"
            "    if (cur != RUNNING) return 1;\n"
            "    cur = transition(cur);\n"
            "    if (cur != PAUSED) return 2;\n"
            "    cur = transition(cur);\n"
            "    if (cur != STOPPED) return 3;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 4: Named enum inside struct member */
    {
        const char *src =
            "enum Op { ADD = 1, SUB = 2, MUL = 3 };\n"
            "struct Command {\n"
            "    enum Op opcode;\n"
            "    int operand;\n"
            "};\n"
            "int main(void) {\n"
            "    struct Command cmd;\n"
            "    cmd.opcode = MUL;\n"
            "    cmd.operand = 7;\n"
            "    int res = 0;\n"
            "    if (cmd.opcode == MUL) res = cmd.operand * 6;\n"
            "    if (res != 42) return 1;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 5: Typedef of named enum and pointer to enum */
    {
        const char *src =
            "enum Direction { NORTH = 10, SOUTH = 20, EAST = 30, WEST = 40 };\n"
            "typedef enum Direction Dir;\n"
            "void turn_around(Dir *d) {\n"
            "    if (*d == NORTH) *d = SOUTH;\n"
            "    else if (*d == SOUTH) *d = NORTH;\n"
            "}\n"
            "int main(void) {\n"
            "    Dir heading = NORTH;\n"
            "    turn_around(&heading);\n"
            "    if (heading != SOUTH) return 1;\n"
            "    turn_around(&heading);\n"
            "    if (heading != NORTH) return 2;\n"
            "    return 42;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 6: Block-scoped named enum and conditional comparison */
    {
        const char *src =
            "int main(void) {\n"
            "    enum Mode { MODE_A = 100, MODE_B = 200, MODE_C = 300 };\n"
            "    enum Mode m = MODE_B;\n"
            "    int val = 0;\n"
            "    if (m == MODE_A) val = 1;\n"
            "    else if (m == MODE_B) val = 42;\n"
            "    else val = 3;\n"
            "    return val;\n"
            "}\n";
        int res = run_snippet(src);
        assert(res == 42);
    }

    /* Test 7: Enum constant auto-increment overflow past INT64_MAX */
    {
        const char *src =
            "enum Big { BIG_A = 9223372036854775807LL, BIG_B };\n"
            "int main(void) { return 0; }\n";
        ZeroCCCompiler *comp = calloc(1, sizeof(ZeroCCCompiler));
        char *prep = zero_cc_preprocess(src, NULL);
        zero_cc_tokenize_c99(prep, comp);
        free(prep);
        zero_cc_compile_c99(comp);
        assert(comp->had_error != 0);
        assert(strstr(comp->error_msg, "overflow") != NULL);
               zero_cc_compiler_free(comp);
    }

    printf("All Named Enum Type-Tag JIT tests passed!\n");
    return 0;
}
