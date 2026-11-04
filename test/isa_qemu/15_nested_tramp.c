/* GNU nested-function trampolines (Sprint 97): capturing nested functions
 * called through function pointers, across all target ISAs.
 *
 * Covers the trampoline stub's argument streams:
 *   - by-reference scalar captures (int + double) passed as GPR pointers
 *   - FP user args (register window + the shared caller-stack stream)
 *   - >8 integer user args (entry + final stack overflow)
 *   - two-level nesting (a capture that is itself a capture mirror)
 *
 * isa-skip: riscv32 - trampoline stubs are machine-code path and land on
 * x86-64 / AArch64 / RISC-V64; the RV32 backend rejects capturing nested
 * functions with a clean diagnostic (documented limitation).
 */
#include <stdio.h>

/* FP capture + FP + many-int user args (stack overflow both ways). */
static int mixed(int x, int *out)
{
    int cap = x;
    double capd = x * 0.5;
    int inm(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            double p, double q)
    {
        int isum = a + b + c + d + e + f + g + h + i;
        int dsum = (int)(p + q);
        *out = isum + dsum + cap + (int)capd;
        return 0;
    }
    int (*fp)(int, int, int, int, int, int, int, int, int, double, double) = inm;
    fp(1, 2, 3, 4, 5, 6, 7, 8, 9, 10.0, 20.0);
    return 0;
}

/* double capture (by-ref) + double user arg. */
static double fpcap(double d, double *out)
{
    double s = d * 2.0;
    double inn(double a) { *out = a + s; return 0; }
    double (*fp)(double) = inn;
    fp(41.5);
    return 0;
}

/* two-level nesting through function pointers at both levels. */
static int nest2(int x, int *out)
{
    int a = x + 1;
    int mid(int p)
    {
        int b = p + a;
        int deep(int q) { *out = q + b + a; return 0; }
        int (*fp)(int) = deep;
        fp(10);
        return b;
    }
    int (*fpm)(int) = mid;
    int mb = fpm(5);
    *out += mb;
    return 0;
}

int main(void)
{
    int r1 = 0;
    double r2 = 0;
    int r3 = 0;
    mixed(1, &r1);       /* 45 + 30 + 1 + 0 = 76 */
    fpcap(0.5, &r2);     /* 41.5 + 1.0 = 42.5 */
    nest2(1, &r3);       /* deep: 10 + 7 + 2 = 19; mid: 7 -> 19 + 7 = 26 */
    printf("r1=%d r2=%g r3=%d\n", r1, r2, r3);
    int ok = (r1 == 76) && (r2 == 42.5) && (r3 == 26);
    return ok ? 0 : 1;
}
