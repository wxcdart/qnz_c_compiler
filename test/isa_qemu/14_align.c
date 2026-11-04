/* C11 _Alignas(N>16) on automatic objects (Sprint 96): the prologue must
 * realign SP to N (x86-64: and $-N, rsp; aarch64: mask via x16; riscv: andi)
 * so the local is actually N-aligned, with _Alignof reporting N and the
 * value working across calls. N covers the andi/and-mask boundary on each
 * ISA (32/64 everywhere; 4096 exercises the 12-bit riscv immediate). */
#include <stdio.h>

static int use(int x){ return x + 1; }

static int f32(void){
    _Alignas(32) int a;
    a = use(41);
    printf("32: aligned=%d alignof=%zu val=%d\n",
           ((size_t)&a % 32) == 0, (size_t)_Alignof(a), a);
    return ((size_t)&a % 32) == 0 && _Alignof(a) == 32 && a == 42;
}

static int f64(void){
    _Alignas(64) int b;
    b = use(10);
    printf("64: aligned=%d alignof=%zu val=%d\n",
           ((size_t)&b % 64) == 0, (size_t)_Alignof(b), b);
    return ((size_t)&b % 64) == 0 && _Alignof(b) == 64 && b == 11;
}

struct S { _Alignas(64) double v; int pad; };
static int fstruct(int a, double d){
    struct S s;
    s.v = d; s.pad = a;
    printf("struct: aligned=%d alignof=%zu val=%.1f pad=%d\n",
           ((size_t)&s % 64) == 0, (size_t)_Alignof(s), s.v, s.pad);
    return ((size_t)&s % 64) == 0 && _Alignof(s) == 64 && s.pad == a;
}

static int f4096(void){
    _Alignas(4096) char buf[16];
    buf[0] = 7; buf[15] = 9;
    printf("4096: aligned=%d alignof=%zu val=%d%d\n",
           ((size_t)buf % 4096) == 0, (size_t)_Alignof(buf), buf[0], buf[15]);
    return ((size_t)buf % 4096) == 0 && _Alignof(buf) == 4096 && buf[0] == 7 && buf[15] == 9;
}

int main(void){
    int ok = 1;
    ok = ok && f32();
    ok = ok && f64();
    ok = ok && fstruct(5, 2.5);
    ok = ok && f4096();
    /* repeated entry: the realign amount varies per call site */
    for (int i = 0; i < 4; i++) ok = ok && f64();
    return ok ? 0 : 1;
}
