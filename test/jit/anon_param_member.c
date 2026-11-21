#include <stdio.h>

/* 1. Direct member read and local mutation on by-value anonymous struct param */
static long read_and_mutate_val(struct { int a; long b; } p) {
    p.a += 5;
    p.b *= 2;
    return (long)p.a + p.b;
}

/* 2. Pointer to anonymous struct parameter + member read and mutation */
static int sum_and_bump_ptr(struct { int x; int y; } *p) {
    p->x += 10;
    return p->x + p->y;
}

/* 3. Anonymous union parameter */
static unsigned int read_union(union { unsigned int u; unsigned char b[4]; } u) {
    return u.u + (unsigned int)u.b[0];
}

/* 4. Inline named struct parameter */
static int use_inline_named(struct InlinePair { int k; int v; } p) {
    struct InlinePair copy = p;
    return copy.k * 100 + copy.v;
}

/* 5. Inline enum parameter */
static int eval_mode(enum { MODE_ADD = 1, MODE_MUL = 2 } mode, int x, int y) {
    if (mode == MODE_ADD) return x + y;
    if (mode == MODE_MUL) return x * y;
    return -1;
}

/* 6. Nested anonymous struct (named field) inside anonymous struct parameter */
static int nested_anon_member(struct { int id; struct { int x; int y; } pt; } s) {
    __typeof__(s) local_copy = s;
    if ((int)sizeof(s) <= 0 || (int)sizeof(s) != (int)sizeof(local_copy)) {
        return -1;
    }
    return local_copy.id + local_copy.pt.x * 10 + local_copy.pt.y;
}

/* 7. Multi-parameter anonymous structs in the same signature */
static int multi_anon_params(struct { int a; } x, struct { long b; int c; } y) {
    return x.a + (int)y.b + y.c;
}

/* 8. Compound literal passed directly to anonymous struct parameter */
static int compound_lit_callee(struct { int a; long b; } p) {
    __typeof__(p) tmp = p;
    return tmp.a + (int)tmp.b;
}

int main(void) {
#ifdef __ZEROCC__
    /* Verify Test 1: by-value read & mutation */
    long r1 = read_and_mutate_val((__typeof__((struct { int a; long b; }){0, 0})){ 7, 15L });
    if (r1 != 42L) {
        printf("FAIL test1: r1=%ld\n", r1);
        return 1;
    }

    /* Verify Test 2: pointer member read & mutation */
    struct { int x; int y; } pt = { 3, 4 };
    int r2 = sum_and_bump_ptr((void *)&pt);
    if (r2 != 17 || pt.x != 13 || pt.y != 4) {
        printf("FAIL test2: r2=%d pt=(%d,%d)\n", r2, pt.x, pt.y);
        return 1;
    }

    /* Verify Test 3: anonymous union param */
    unsigned int r3 = read_union((__typeof__((union { unsigned int u; unsigned char b[4]; }){0})){ 0x00000102u });
    if (r3 != 0x00000104u) {
        printf("FAIL test3: r3=0x%x\n", r3);
        return 1;
    }

    /* Verify Test 4: inline named struct param + later reference to InlinePair */
    struct InlinePair ip = { 4, 29 };
    int r4 = use_inline_named(ip);
    if (r4 != 429) {
        printf("FAIL test4: r4=%d\n", r4);
        return 1;
    }

    /* Verify Test 5: inline enum param */
    if (eval_mode(MODE_ADD, 6, 7) != 13 || eval_mode(MODE_MUL, 6, 7) != 42) {
        printf("FAIL test5: enum mode\n");
        return 1;
    }

    /* Verify Test 6: nested anonymous struct param, sizeof, and __typeof__ */
    int r6 = nested_anon_member(
        (__typeof__((struct { int id; struct { int x; int y; } pt; }){0, {0, 0}})){
            100, { 3, 7 }
        });
    if (r6 != 137) {
        printf("FAIL test6: r6=%d\n", r6);
        return 1;
    }

    /* Verify Test 7: multi-parameter anonymous structs */
    int r7 = multi_anon_params(
        (__typeof__((struct { int a; }){0})){ 5 },
        (__typeof__((struct { long b; int c; }){0, 0})){ 10L, 20 });
    if (r7 != 35) {
        printf("FAIL test7: r7=%d\n", r7);
        return 1;
    }

    /* Verify Test 8: compound literal directly passed (using compatible anonymous layout) */
    int r8 = compound_lit_callee((struct { int a; long b; }){ 11, 22L });
    if (r8 != 33) {
        printf("FAIL test8: r8=%d\n", r8);
        return 1;
    }
#else
    /* Standard C host compiler treats each unnamed struct as distinct at file scope,
     * so invoke by-value callees via layout-compatible typedefs for differential runs. */
    typedef struct { int a; long b; } PairAB;
    typedef union { unsigned int u; unsigned char b[4]; } UVal;
    typedef struct { int k; int v; } PairKV;
    typedef struct { int id; struct { int x; int y; } pt; } NestedS;
    typedef struct { int a; } WrapA;
    typedef struct { long b; int c; } PairBC;

    long r1 = ((long (*)(PairAB))(void *)read_and_mutate_val)((PairAB){ 7, 15L });
    if (r1 != 42L) return 1;

    struct { int x; int y; } pt = { 3, 4 };
    int r2 = sum_and_bump_ptr((void *)&pt);
    if (r2 != 17 || pt.x != 13 || pt.y != 4) return 1;

    unsigned int r3 = ((unsigned int (*)(UVal))(void *)read_union)((UVal){ 0x00000102u });
    if (r3 != 0x00000104u) return 1;

    int r4 = ((int (*)(PairKV))(void *)use_inline_named)((PairKV){ 4, 29 });
    if (r4 != 429) return 1;

    if (((int (*)(int, int, int))(void *)eval_mode)(1, 6, 7) != 13 ||
        ((int (*)(int, int, int))(void *)eval_mode)(2, 6, 7) != 42) {
        return 1;
    }

    int r6 = ((int (*)(NestedS))(void *)nested_anon_member)((NestedS){ 100, { 3, 7 } });
    if (r6 != 137) return 1;

    int r7 = ((int (*)(WrapA, PairBC))(void *)multi_anon_params)(
        (WrapA){ 5 }, (PairBC){ 10L, 20 });
    if (r7 != 35) return 1;

    int r8 = ((int (*)(PairAB))(void *)compound_lit_callee)((PairAB){ 11, 22L });
    if (r8 != 33) return 1;
#endif

    printf("anon_param_member passed\n");
    return 0;
}
