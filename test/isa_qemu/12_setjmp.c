#include <setjmp.h>
#include <stdio.h>

static jmp_buf g_env;

static int clobber_and_jump(jmp_buf env, int val, int depth) {
    int a = depth + 11;
    int b = depth + 22;
    int c = depth + 33;
    int d = depth + 44;
    int e = depth + 55;
    int f = depth + 66;
    if (depth > 0) {
        return clobber_and_jump(env, val, depth - 1) + a + b + c + d + e + f;
    }
    longjmp(env, val);
    return -1;
}

static int calc(int x) { return x * 3 + 7; }

static int test_local_setjmp(int seed) {
    jmp_buf env;
    int live = calc(seed);
    volatile int mod = 10;
    int r = setjmp(env);
    if (r == 0) {
        mod = 25;
        clobber_and_jump(env, 42, 3);
        return -1;
    }
    return live + mod + r;
}

static int test_zero_and_loop(void) {
    volatile int step = 0;
    int r = setjmp(g_env);
    if (step == 0) {
        step = 1;
        clobber_and_jump(g_env, 0, 2);
        return -1;
    }
    if (step == 1) {
        if (r != 1) return -2;
        step = 2;
        clobber_and_jump(g_env, 9, 2);
        return -3;
    }
    return r + step;
}

int main(void) {
    int c1 = calc(1);
    int c2 = calc(2);
    int c3 = calc(3);
    int c4 = calc(4);
    int a = test_local_setjmp(5);
    int b = test_zero_and_loop();
    printf("setjmp=%d loop=%d cs=%d\n", a, b, c1 + c2 + c3 + c4);
    return 0;
}
