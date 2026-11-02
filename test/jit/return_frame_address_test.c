/* test/jit/return_frame_address_test.c — __builtin_return_address, __builtin_frame_address, and empty structs */

extern void abort(void);
extern void exit(int);

/* Test empty structs */
struct Empty {};
union EmptyUnion {};

struct Container {
    int a;
    struct Empty e;
    int b;
};

struct Empty pass_empty(struct Empty e) {
    return e;
}

static void *ret_addr_0;
static void *ret_addr_1;

__attribute__((noinline))
void *helper(void) {
    void *fa = __builtin_frame_address(0);
    void *ra = __builtin_return_address(0);
    if (!fa || !ra) abort();
    ret_addr_0 = ra;
    return fa;
}

__attribute__((noinline))
void *caller_helper(void) {
    void *fa = helper();
    void *ra1 = __builtin_return_address(0);
    ret_addr_1 = ra1;
    return fa;
}

__attribute__((noinline))
void *nested_return_address(void) {
    return __builtin_return_address(1);
}

__attribute__((noinline))
void *call_nested(void) {
    return nested_return_address();
}

int main(void) {
    /* 1. Empty struct size and passing */
    if (sizeof(struct Empty) != 0) abort();
    if (sizeof(union EmptyUnion) != 0) abort();
    if (sizeof(struct Container) != 8) abort();

    struct Empty e1;
    struct Empty e2 = pass_empty(e1);
    (void)e2;

    struct Container c = { 10, {}, 20 };
    if (c.a != 10 || c.b != 20) abort();

    /* 2. __builtin_frame_address(0) */
    void *my_fa = __builtin_frame_address(0);
    if (!my_fa) abort();

    /* 3. __builtin_return_address(0) */
    void *callee_fa = caller_helper();
    if (!callee_fa) abort();
    /* callee frame address must not equal caller frame address */
    if (callee_fa == my_fa) abort();

    /* 4. Level 1 return address */
    void *expected_ra = call_nested();
    if (!expected_ra) abort();

    return 0;
}
