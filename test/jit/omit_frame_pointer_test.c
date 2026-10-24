#include <stdio.h>
#include <assert.h>

/* Frame pointer omission regression test (Sprint 44, Option 4).
 *
 * At the default optimization level (-O1+) zerocc omits the frame pointer
 * for functions with fully static frames and addresses all slots from SP
 * with a per-function displacement. These exercises keep the geometry
 * honest across the sweep (-O0..-O3, JIT + AOT):
 *   - fixed-frame non-leaf functions (FP-less prologue/epilogue + rebased
 *     local/parameter slots),
 *   - recursion with callee-saved register pressure (CSR save/restore at
 *     SP-based offsets must stay symmetric),
 *   - deep call chains (nested FP-less frames),
 *   - leaf functions with frames (x86-64 red-zone case, no SP adjust),
 *   - functions with runtime VLAs (must KEEP the frame pointer and work),
 *   - a >4 KiB frame in a calling function (x86-64 stack-probe path,
 *     AArch64 add-via-x16 epilogue path).
 */

int add(int a, int b) { return a + b; }

int frame_user(int a, int b) {
    int x0 = a + 1;
    int x1 = b + 2;
    int x2 = x0 * x1;
    int x3 = x2 + a;
    int x4 = x3 - b;
    int x5 = x4 * 2;
    int h = add(x5, 7);   /* call with many live temporaries -> CSR pressure */
    return x0 + x1 + x2 + x3 + x4 + x5 + h;
}

int recursive(int n) { if (n <= 1) return 1; return n * recursive(n - 1); }

int deep1(int n) { return frame_user(n, n + 1) + n; }
int deep2(int n) { return deep1(n) * 2; }
int deep3(int n) { return deep2(n) - deep1(n - 1); }

int leaf_arith(int a, int b, int c, int d) {
    int x = (a + b) * (c - d);
    return x;
}

int vla_sum(int n) {
    int v[n];
    for (int i = 0; i < n; i++) v[i] = i * i;
    int s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return s;
}

int big_frame(int n) {
    char buf[5000];
    for (int i = 0; i < 5000; i++) buf[i] = (char)i;
    int s = 0;
    for (int i = 0; i < n; i++) s += buf[i];
    return s + add(n, 0);
}

int main(void) {
    assert(frame_user(2, 3) == (3) + (5) + 15 + 17 + 14 + 28 + 35);
    assert(recursive(6) == 720);
    assert(deep3(4) == deep2(4) - deep1(3));
    assert(deep3(4) == 2 * (frame_user(4, 5) + 4) - (frame_user(3, 4) + 3));
    assert(leaf_arith(1, 2, 10, 4) == 18);
    assert(leaf_arith(7, 1, 9, 9) == 0);
    assert(vla_sum(0) == 0);
    assert(vla_sum(1) == 0);
    assert(vla_sum(10) == 285);      /* sum of squares 0..9 */
    assert(vla_sum(20) == 2470);
    assert(big_frame(0) == 0);
    assert(big_frame(10) == 45 + 10);
    assert(big_frame(50) == 1225 + 50);
    printf("omit_frame_pointer_test: all tests passed\n");
    return 0;
}
