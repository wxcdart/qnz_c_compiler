/* Test suite for Whole-Program Interprocedural Dead Function & Global Data Stripping (IPO) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL: %s (expected %ld, got %ld)\n", msg, (long)(b), (long)(a)); \
        exit(1); \
    } \
} while (0)

/* 1. Dead static function that is never called */
static int dead_func_unused(int x) {
    return x * 100 + 42;
}

/* 2. Dead static global variable that is never referenced */
static int dead_global_unused = 98765;
static const char *dead_str_unused = "never used string";

/* 3. Inlined static function */
static int inlined_helper(int a, int b) {
    return a * 2 + b * 3;
}

/* 4. Static function call chain */
static int chain_c(int x) { return x + 10; }
static int chain_b(int x) { return chain_c(x) * 2; }
static int chain_a(int x) { return chain_b(x) + 5; }

/* 5. Address-taken static function */
static int callback_target(int x) {
    return x * 7;
}

static int (*get_callback(int flag))(int) {
    if (flag) return callback_target;
    return NULL;
}

/* 6. Static global variables that are live */
static int live_g1 = 123;
static int live_g2 = 456;
static int *live_gptr = &live_g2;

typedef struct {
    int id;
    int (*op)(int);
    int *data;
} Handler;

static int handler_op(int v) {
    return v + 1000;
}

static Handler global_handlers[] = {
    { 1, handler_op, &live_g1 },
};

int main(void) {
    /* Test 1: Inlined helper execution */
    int r1 = inlined_helper(10, 20);
    ASSERT_EQ(r1, 80, "inlined_helper(10, 20)");

    /* Test 2: Static call chain */
    /* chain_a(5) = chain_b(5) + 5 = (chain_c(5)*2) + 5 = ((5+10)*2) + 5 = 35 */
    int r2 = chain_a(5);
    ASSERT_EQ(r2, 35, "chain_a(5)");

    /* Test 3: Address-taken static function */
    int (*cb)(int) = get_callback(1);
    ASSERT_EQ(cb != NULL, 1, "get_callback(1) != NULL");
    int r3 = cb(6);
    ASSERT_EQ(r3, 42, "cb(6)");

    /* Test 4: Live global variables */
    ASSERT_EQ(live_g1, 123, "live_g1");
    ASSERT_EQ(*live_gptr, 456, "*live_gptr");

    /* Test 5: Global handler struct pointing to static func and static var */
    ASSERT_EQ(global_handlers[0].id, 1, "global_handlers[0].id");
    ASSERT_EQ(global_handlers[0].op(50), 1050, "global_handlers[0].op(50)");
    ASSERT_EQ(*global_handlers[0].data, 123, "*global_handlers[0].data");

    printf("ALL IPO DEAD STRIP TESTS PASSED\n");
    return 0;
}
