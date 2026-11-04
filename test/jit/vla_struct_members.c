/* Sprint 100: GNU variably-modified struct/union members (local
 * structs with runtime-sized array members).
 *
 * Covers the lowering paths that assume a frame-relative struct layout:
 *   1. Member store/read on a VLA struct local (the frame slot holds the
 *      runtime-allocated OBJECT POINTER, not the object itself - a
 *      frame-relative store clobbers the saved pointer).
 *   2. Member array decay (x.a as pointer) for local VLA structs.
 *   3. Definition-time dimension capture (GCC evaluates VLA member
 *      dimensions when the struct definition executes, not at sizeof /
 *      allocation time): mutating the dimension variable afterwards must
 *      not change sizeof(struct s).
 *   4. Packed block-scope VLA struct definition with no declarator.
 *   5. va_arg(ap, typeof(d)) for an anonymous VLA struct (two GP units:
 *      object pointer + runtime size).
 *   6. VLA struct value assignment (runtime-sized memcpy).
 *   7. Arrays of VLA structs (runtime element stride).
 *   8. By-value VLA struct parameter of a nested function (c-torture
 *      compile/nested-1 shape; here executed directly).
 *   9. Constant sizeof() array dimensions must not be misclassified as
 *      VLA members (c-torture 20111208-1 / 20111212-1 / 991014-1).
 *
 * Compiled with zerocc (AOT O0-O3 + JIT) and host gcc (gnu17); expected
 * exit code 0. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* 1 + 8: named block-scope VLA struct, member store/read + by-value
 * nested parameter. */
static int t1 (int argc)
{
    struct s1 { int a; char b[argc]; };
    int nested (struct s1 x) { return x.a + (int)sizeof(x); }
    struct s1 t;
    memset(&t, 0, sizeof(t));
    t.a = 123;
    if (t.a != 123) return 1;
    /* runtime size: int (4) + char[argc] padded to align(4) */
    {
        size_t expect = (size_t)argc;
        expect = (expect + 3) & ~(size_t)3;
        if (sizeof(t) != 4 + expect) return 3;
    }
    if (nested(t) != 123 + (int)sizeof(t)) return 4;
    return 0;
}

/* 2: member array decay on a local (anonymous) VLA struct. */
static int t2 (int argc)
{
    int z = argc;
    struct { char a[z]; } x;
    x.a[0] = 65;
    x.a[1] = 66;
    x.a[z - 1] = 90;
    if (x.a[0] != 65 || x.a[1] != 66 || x.a[z - 1] != 90) return 1;
    return 0;
}

/* 3: definition-time dimension capture. */
static int t3 (int n)
{
    struct s3 { char b[n]; } __attribute__((packed));
    int at_def = n;
    n++;
    if (sizeof(struct s3) != (size_t)at_def) return 1;
    {
        struct s3 tt;
        if (sizeof(tt) != (size_t)at_def) return 2;
    }
    return 0;
}

/* 4: packed VLA struct definition with no declarator at all. */
static int t4 (int n)
{
    struct s4 { char q[n]; } __attribute__((packed));
    (void)n;
    return (int)sizeof(struct s4) == n ? 0 : 1;
}

/* 5: va_arg of an anonymous VLA struct. */
static int va_bad = 0;

static void va_foo (int size, ...)
{
    struct { char x[size]; } d;
    va_list ap;
    int i;
    va_start(ap, size);
    d = va_arg(ap, typeof(d));
    for (i = 0; i < size; i++) {
        if (d.x[i] != (char)('0' + i)) va_bad = 1;
    }
    d = va_arg(ap, typeof(d));
    for (i = 0; i < size; i++) {
        if (d.x[i] != (char)('0' + size + i)) va_bad = 2;
    }
    va_end(ap);
}

static int t5 (void)
{
    int z = 5;
    struct { char a[z]; } x, y;
    int i;
    for (i = 0; i < z; i++) x.a[i] = '0' + i;
    for (i = 0; i < z; i++) y.a[i] = '0' + z + i;
    va_foo(z, x, y);
    return va_bad;
}

/* 6: VLA struct value assignment. */
static int t6 (int argc)
{
    struct s6 { int a; char b[argc]; };
    struct s6 a, b;
    int i;
    memset(&a, 0, sizeof(a));
    a.a = 7;
    for (i = 0; i < argc; i++) a.b[i] = (char)(i + 1);
    b = a;
    if (b.a != 7) return 1;
    for (i = 0; i < argc; i++) if (b.b[i] != (char)(i + 1)) return 2;
    /* mutate source, target must keep its copy */
    a.a = 8;
    if (b.a != 7) return 3;
    return 0;
}

/* 7: array of VLA structs - runtime element stride. */
static int t7 (int argc)
{
    int z = argc;
    struct { char v[z]; } arr[3];
    int r, i;
    for (r = 0; r < 3; r++)
        for (i = 0; i < z; i++)
            arr[r].v[i] = (char)(r * 10 + i);
    for (r = 0; r < 3; r++)
        for (i = 0; i < z; i++)
            if (arr[r].v[i] != (char)(r * 10 + i)) return 1;
    return 0;
}

/* 9: constant sizeof() dimensions - NOT VLA members. */
struct fix16 { char a[sizeof(int) * 4]; };
static int t9 (void)
{
    struct fix16 f;
    if (sizeof(f) != 16) return 1;
    f.a[0] = 1;
    f.a[15] = 2;
    if (f.a[0] != 1 || f.a[15] != 2) return 2;
    return 0;
}

int main (void)
{
    if (t1(5) != 0) return 10;
    if (t1(3) != 0) return 11;
    if (t2(6) != 0) return 20;
    if (t3(123) != 0) return 30;
    if (t3(1) != 0) return 31;
    if (t4(9) != 0) return 40;
    if (t5() != 0) return 50;
    if (t6(4) != 0) return 60;
    if (t7(6) != 0) return 70;
    if (t9() != 0) return 90;
    return 0;
}
