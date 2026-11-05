/* Sprint 110: __int128 / __uint128_t pass-by-value ARGUMENT ABI.
 *
 * A 128-bit integer argument occupies TWO consecutive GP argument units
 * (System V AMD64 rdi:rsi / rdx:rcx / r8:r9, AAPCS64 xN:xN+1, RISC-V
 * aN:aN+1): low half in the first register, high half in the second.
 * Beyond the register window the 16 bytes ride the caller stack in
 * argument order (low unit at the lower address).
 *
 * Exercised: first/second/third argument position, adjacent narrow args
 * on both sides, 128-bit args interleaved with FP args (independent
 * streams), two 128-bit args at once, signed and unsigned types, narrow
 * integer conversions (zero/sign-extended), 64-bit truncation, compound
 * expression args, global 128-bit objects, and register-window overflow
 * (x86-64 has 6 GP units: a 128-bit 4th+ arg overflows to the stack).
 * Checked via >>64 + 64-bit compares (the 128-bit return ABI is covered
 * by int128_return.c). Runs under test-opt (JIT + AOT, -O0..-O3) and the
 * cross-ISA isa128 gate; excluded from the host-gcc `make test` baseline.
 */
__uint128_t g_u = 0x0000AABBCCDDEEFF00ULL + (((__uint128_t)0x1122334455667788ULL) << 64);
__int128    g_s = -42 - (((__int128)1) << 64);   /* (0xFFFFFFFFFFFFFFD6, 0xFFFFFFFFFFFFFFFE) */

__uint128_t first_u(__uint128_t a) { return a; }
__uint128_t second_u(__uint128_t a, __uint128_t b) { return b; }
__uint128_t third_u(int x, int y, __uint128_t z) { return z; }
__uint128_t mixed_u(int before, __uint128_t a, double d, __uint128_t b) {
    (void)d;
    return a + b;
}
__uint128_t both_u(__uint128_t a, __uint128_t b) { return a ^ b; }
__uint128_t sext_u(__int128 a) { return (__uint128_t)a; }
__uint128_t zext_u(unsigned long a) { return a; }
__uint128_t trunc_u(unsigned long long a) { return a; }
__uint128_t expr_u(__uint128_t a) { return a + 1; }
__uint128_t overflow_u(int a, int b, int c, __uint128_t d) { return d; }
/* 5 int args fill GP units 0-4; the 128-bit 6th arg takes units 5-6:
 * on x86-64 (6 units) the LOW unit stays in a register and the HIGH unit
 * overflows to the caller stack - the pair straddles the window. On
 * AArch64/RISC-V (8 units) both stay in registers. */
__uint128_t straddle_u(int a, int b, int c, int d, int e, __uint128_t f) { return f; }
__uint128_t fp_mix_u(__uint128_t a, double d1, double d2, __uint128_t b) {
    (void)d1; (void)d2;
    return a - b;
}

#define LO(x) ((unsigned long long)(x))
#define HI(x) (((__uint128_t)(x)) >> 64)

int main(void) {
    int ok = 1;

    /* Argument position 1 (rdi:rsi / x0:x1 / a0:a1) */
    {
        __uint128_t v = 0x0123456789ABCDEFULL + (((__uint128_t)0xFEDCBA9876543210ULL) << 64);
        __uint128_t r = first_u(v);
        if (LO(r) != 0x0123456789ABCDEFULL) ok = 0;
        if (HI(r) != 0xFEDCBA9876543210ULL) ok = 0;
    }

    /* Argument position 2, with a narrow arg in front (stream order) */
    {
        __uint128_t v = 0x1111111111111111ULL + (((__uint128_t)0x2222222222222222ULL) << 64);
        __uint128_t r = second_u(7, v);
        if (LO(r) != 0x1111111111111111ULL) ok = 0;
        if (HI(r) != 0x2222222222222222ULL) ok = 0;
    }

    /* Argument position 3, two narrow args in front */
    {
        __uint128_t v = 0x3333333333333333ULL + (((__uint128_t)0x4444444444444444ULL) << 64);
        __uint128_t r = third_u(1, 2, v);
        if (LO(r) != 0x3333333333333333ULL) ok = 0;
        if (HI(r) != 0x4444444444444444ULL) ok = 0;
    }

    /* Narrow arg AFTER a 128-bit arg (the unit index must skip the pair) */
    {
        __uint128_t v = 0x5555555555555555ULL + (((__uint128_t)0x6666666666666666ULL) << 64);
        __uint128_t r = mixed_u(0, v, 1.5, 0x9999999999999999ULL);
        if (LO(r) != 0xEEEEEEEEEEEEEEEEULL) ok = 0;   /* 0x5555.. + 0x9999.. */
        if (HI(r) != 0x6666666666666666ULL) ok = 0;
    }

    /* Two 128-bit args at once (four GP units) */
    {
        __uint128_t a = 0xAAAAAAAAAAAAAAAAULL + (((__uint128_t)0xBBBBBBBBBBBBBBBBULL) << 64);
        __uint128_t b = 0xCCCCCCCCCCCCCCCCULL + (((__uint128_t)0xDDDDDDDDDDDDDDDDULL) << 64);
        __uint128_t r = both_u(a, b);
        if (LO(r) != 0x6666666666666666ULL) ok = 0;
        if (HI(r) != 0x6666666666666666ULL) ok = 0;
    }

    /* Signed 128-bit argument (negative: hi must stay sign-extended) */
    {
        __int128 s = -42 - (((__int128)1) << 64);
        __uint128_t r = sext_u(s);
        if (LO(r) != 0xFFFFFFFFFFFFFFD6ULL) ok = 0;
        if (HI(r) != 0xFFFFFFFFFFFFFFFEULL) ok = 0;
    }

    /* Narrow integer conversions into a 128-bit parameter */
    {
        __uint128_t r = sext_u(-7);                    /* signed: (F9, FF) */
        if (LO(r) != 0xFFFFFFFFFFFFFFF9ULL) ok = 0;
        if (HI(r) != 0xFFFFFFFFFFFFFFFFULL) ok = 0;
        r = sext_u(1000);                              /* positive: (3E8, 0) */
        if (LO(r) != 1000ULL) ok = 0;
        if (HI(r) != 0) ok = 0;
        r = zext_u(0xDEADBEEFULL);                     /* unsigned: zext */
        if (LO(r) != 0xDEADBEEFULL) ok = 0;
        if (HI(r) != 0) ok = 0;
    }

    /* 128-bit argument to a 64-bit parameter: low-half truncation */
    {
        __uint128_t v = 0xCAFEBABECAFEBABEULL + (((__uint128_t)0xFEDCBA9876543210ULL) << 64);
        __uint128_t r = trunc_u(v);        if (LO(r) != 0xCAFEBABECAFEBABEULL) ok = 0;
        if (HI(r) != 0) ok = 0;
    }

    /* Compound expression argument (evaluated, then passed as a pair) */
    {
        __uint128_t v = 0x0F0F0F0F0F0F0F0FULL + (((__uint128_t)0x1F1F1F1F1F1F1F1FULL) << 64);
        __uint128_t r = expr_u(v);
        if (LO(r) != 0x0F0F0F0F0F0F0F10ULL) ok = 0;
        if (HI(r) != 0x1F1F1F1F1F1F1F1FULL) ok = 0;
    }

    /* 128-bit global object passed by value */
    {
        __uint128_t r = first_u(g_u);
        if (LO(r) != 0x0000AABBCCDDEEFF00ULL) ok = 0;
        if (HI(r) != 0x1122334455667788ULL) ok = 0;
        __uint128_t rs = sext_u(g_s);
        if (LO(rs) != 0xFFFFFFFFFFFFFFD6ULL) ok = 0;
        if (HI(rs) != 0xFFFFFFFFFFFFFFFEULL) ok = 0;
    }

    /* Register-window overflow: on x86-64 (6 GP units) the 128-bit 4th
     * argument lands on the caller stack; on AArch64/RISC-V (8 units) it
     * stays in registers. Either way the pair must arrive intact. */
    {
        __uint128_t v = 0x7777777777777777ULL + (((__uint128_t)0x8888888888888888ULL) << 64);
        __uint128_t r = overflow_u(1, 2, 3, v);
        if (LO(r) != 0x7777777777777777ULL) ok = 0;
        if (HI(r) != 0x8888888888888888ULL) ok = 0;
    }

    /* Pair straddling the register/stack boundary (x86-64: lo in a
     * register, hi on the caller stack). */
    {
        __uint128_t v = 0x9999999999999999ULL + (((__uint128_t)0xAAAAAAAAAAAAAAAAULL) << 64);
        __uint128_t r = straddle_u(1, 2, 3, 4, 5, v);
        if (LO(r) != 0x9999999999999999ULL) ok = 0;
        if (HI(r) != 0xAAAAAAAAAAAAAAAAULL) ok = 0;
    }

    /* 128-bit args interleaved with FP args (independent streams) */
    {
        __uint128_t a = 0x0102030405060708ULL + (((__uint128_t)0x090A0B0C0D0E0F10ULL) << 64);
        __uint128_t b = 0x0000000000000001ULL;
        __uint128_t r = fp_mix_u(a, 2.5, 3.5, b);
        if (LO(r) != 0x0102030405060707ULL) ok = 0;
        if (HI(r) != 0x090A0B0C0D0E0F10ULL) ok = 0;
    }

    return ok ? 0 : 1;
}
