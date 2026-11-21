/* Regression test for -flto single-TU constant global propagation, IPCP,
 * pure const-return folding, and constant-branch pruning (also valid at -O0..O3
 * without -flto). */
static int g_scale = 7;
static int g_unused_flag = 12345;

static int compute_magic(void) {
    return g_scale * 6 - 2;
}

static int apply_mode(int base, int mode) {
    if (mode == 77) {
        return base + compute_magic();
    }
    return base - g_unused_flag;
}

int main(void) {
    int res = (g_scale == 7) ? apply_mode(8, 77) : -99;
    if (g_scale != 7) {
        return 2;
    }
    return (res == 48) ? 0 : 1;
}
