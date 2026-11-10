/* LRA clamp-restore regression pin (Sprint 151).
 *
 * The Sprint 69 va_list-shadow miscompile ("branchy compute/clamp/restore
 * pattern": the restore move elided/merged, the clamped value read back as
 * the exhaustion-test value) was hunted in Sprint 151: the original
 * branchy clamp restored in the shadow produced gcc-matching output for
 * vprintf-1 at -O0..-O3 (JIT + raw AOT + -S text), and 240+ user-code
 * variants were clean. The defect is treated as incidentally fixed by the
 * Sprints 109/117/135 LRA work; THIS FILE PINS THE SHAPE so a future
 * LRA/emit change that regresses the class is caught by make test-opt.
 *
 * Shapes covered (all gcc-compatible, differential vs host gcc):
 *  - all-virtual ternary clamp, both polarities, result used after a call
 *    (spans_call / post-call restore path);
 *  - branchy if/else clamp with the restore copy in each arm position,
 *    result 32-bit-stored to a buffer read back through a function
 *    (defeats store/load forwarding);
 *  - clamp with register pressure (many live values across the branch);
 *  - clamp inside a loop with loop-carried values.
 */
#include <stdio.h>

static int callee(int a, int b) { return a ^ b; }
static int g32(int v) { return v + 2; }
static int g64(long v) { return (int)(v + 3); }

/* All-virtual ternary clamp, result live across a call. */
static int ternary_a(int t, int K, int x, int y) {
    int c = t - K;
    int r = (c < 0) ? K : t;
    r += callee(x, y);
    return r;
}
static int ternary_b(int t, int K, int x, int y) {
    int c = t - K;
    int r = (c >= 0) ? K : t;
    r += callee(x, y);
    return r;
}

/* Branchy clamp, restore in the fall-through arm, 32-bit store to an
 * escaping buffer. */
static int branchy_a(int t, int K) {
    int c = t - K;
    int r;
    if (c < 0)
        r = K;
    else
        r = t;             /* the restore */
    int buf[4];
    buf[0] = (int)r;
    return g32(buf[0]);
}
/* Restore in the jump arm. */
static int branchy_b(int t, int K) {
    int c = t - K;
    int r;
    if (c >= 0)
        r = K;
    else
        r = t;             /* the restore */
    int buf[4];
    buf[0] = (int)r;
    return g32(buf[0]);
}

/* Clamp under pressure: many live values across the branch. */
static int pressured(int t, int K, int a, int b, int d, int e,
                     int f, int g, int h, int j) {
    int x1 = a * 2 + 1, x2 = b * 3 + 2, x3 = d * 5 + 3, x4 = e * 7 + 4;
    int x5 = f * 11 + 5, x6 = g * 13 + 6, x7 = h * 17 + 7, x8 = j * 19 + 8;
    int c = t - K;
    int r = (c < 0) ? K : t;
    r += callee(x1, x2);   /* r live across the call */
    return r + x3 + x4 + x5 + x6 + x7 + x8;
}

/* Clamp inside a loop with loop-carried values. */
static int looped(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        int t = 5 * i + 3;
        int c = t - 48;
        int r = (c < 0) ? 48 : t;
        acc += r + callee(i, i * 3);
    }
    return acc;
}

int main(void) {
    int total = 0;
    for (int i = 0; i < 100; i++) {
        int t = 5 * i + 3;
        total += ternary_a(t, 48, i * 7, i * 11);
        total += ternary_b(t, 48, i * 13, i * 17);
        total += branchy_a(t, 48);
        total += branchy_b(t, 48);
        total += pressured(t, 48, i, i + 1, i + 2, i + 3,
                           i + 4, i + 5, i + 6, i + 7);
        total += looped(12);
        total += g64((long)branchy_a(t, 32) * (long)i);
    }
    printf("%d\n", total);
    return 0;
}
