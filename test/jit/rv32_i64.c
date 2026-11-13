/* Sprint 170 / Phase 1: RV32 64-bit integer arithmetic and value model.
 *
 * Verifies 64-bit (long long / unsigned long long) arithmetic, bitwise logic,
 * shifts, unary operations, comparisons, 32<->64 casts, conditionals, and
 * memory ops (globals, locals, pointers, arrays, structs) on RV32 (two-GPR pairs)
 * without crossing the function calling ABI.
 */

/* Globals */
long long g_a = 0x100000002LL;
unsigned long long g_u = 0xF000000000000001ULL;

struct S {
    int id;
    long long val;
};

struct S g_s = { 42, 0x123456789ABCDEF0LL };

int main(void) {
    /* 1. Globals */
    if (g_a != 0x100000002LL) return 1;
    if (g_u != 0xF000000000000001ULL) return 2;

    /* 2. Locals and arithmetic */
    long long x = 0x100000000LL;
    long long y = 0x20LL;
    long long sum = x + y;
    if (sum != 0x100000020LL) return 3;

    long long diff = sum - y;
    if (diff != 0x100000000LL) return 4;

    long long mul = y * 0x10000LL;
    if (mul != 0x200000LL) return 5;

    /* 3. Logic and bitwise */
    long long and_v = x & 0xFFFFFFFFLL;
    if (and_v != 0) return 6;

    long long or_v = x | 0x55LL;
    if (or_v != 0x100000055LL) return 7;

    long long xor_v = or_v ^ 0x55LL;
    if (xor_v != x) return 8;

    /* 4. Unary operations: NOT, NEG, BANG */
    unsigned long long not_v = ~0ULL;
    if (not_v != 0xFFFFFFFFFFFFFFFFULL) return 9;

    long long neg_v = -1LL;
    if (neg_v + 1LL != 0) return 10;

    long long zero_ll = 0LL;
    if (!zero_ll != 1) return 11;
    if (!x != 0) return 12;

    /* 5. Shifts */
    unsigned long long s1 = 1ULL << 35;
    if (s1 != 0x800000000ULL) return 13;

    unsigned long long s2 = s1 >> 35;
    if (s2 != 1ULL) return 14;

    long long s3 = (-1LL) >> 30; /* SAR */
    if (s3 != -1LL) return 15;

    /* 6. Comparisons (signed vs unsigned) */
    long long neg10 = -10LL;
    long long pos10 = 10LL;
    if (!(neg10 < pos10)) return 16;
    if (!(pos10 > neg10)) return 17;
    if (neg10 >= pos10) return 18;

    unsigned long long u_big = 0xFFFFFFFFFFFFFFFFULL;
    unsigned long long u_small = 10ULL;
    if (!(u_big > u_small)) return 19;
    if (u_big < u_small) return 20;

    /* 7. Casts */
    int i32 = -5;
    long long cast_s64 = (long long)i32;
    if (cast_s64 != -5LL) return 21;

    unsigned int u32 = 0x80000000U;
    unsigned long long cast_u64 = (unsigned long long)u32;
    if (cast_u64 != 0x80000000ULL) return 22;

    long long big64 = 0x1234567887654321LL;
    int trunc_i = (int)big64;
    if (trunc_i != (int)0x87654321) return 23;

    /* 8. Conditions: if, while, for, ternary */
    if (!x) return 24;
    if (zero_ll) return 25;

    long long count = 5LL;
    int loop_c = 0;
    while (count) {
        loop_c++;
        count -= 1LL;
    }
    if (loop_c != 5) return 26;

    long long tern = (x ? 100LL : 200LL);
    if (tern != 100LL) return 27;

    long long tern2 = (zero_ll ? 100LL : 200LL);
    if (tern2 != 200LL) return 28;

    /* 9. Pointers, Arrays, Structs */
    long long arr[3] = { 10LL, 20LL, 30LL };
    if (arr[1] != 20LL) return 29;

    long long *ptr = &arr[2];
    if (*ptr != 30LL) return 30;
    *ptr = 99LL;
    if (arr[2] != 99LL) return 31;

    if (g_s.val != 0x123456789ABCDEF0LL) return 32;
    struct S loc_s;
    loc_s.val = 0xFEEDFACECAFEBEEFLL;
    if (loc_s.val != 0xFEEDFACECAFEBEEFLL) return 33;

    struct S *sp = &loc_s;
    if (sp->val != 0xFEEDFACECAFEBEEFLL) return 34;

    /* 10. Logical AND / OR short-circuit */
    if (!(x && y)) return 35;
    if (zero_ll && x) return 36;
    if (!(zero_ll || x)) return 37;
    if (zero_ll || 0LL) return 38;

    /* 11. 64-bit Division and Modulo (Sprint 173 / Phase 3) */
    long long da = 1000000000000LL;
    long long db = 3000LL;
    if (da / db != 333333333LL) return 39;
    if (da % db != 1000LL) return 40;

    long long sda = -1000000000000LL;
    long long sdb = 7LL;
    if (sda / sdb != -142857142857LL) return 41;
    if (sda % sdb != -1LL) return 42;
    if (sda / -sdb != 142857142857LL) return 43;
    if (da / -sdb != -142857142857LL) return 44;

    unsigned long long uda = 18446744073709551615ULL;
    unsigned long long udb = 1000ULL;
    unsigned long long ud_q = uda / udb;
    unsigned long long ud_r = uda % udb;
    if (ud_q != 18446744073709551ULL) return 45;
    if (ud_r != 615ULL) return 46;
    if (ud_q != 18446744073709551615ULL / 1000ULL) return 47;

    /* Div/rem edge cases: / 1, % 1, x / x, x % x */
    if (da / 1LL != da) return 48;
    if (da % 1LL != 0LL) return 49;
    if (da / da != 1LL) return 50;
    if (da % da != 0LL) return 51;
    if (uda / 1ULL != uda) return 52;
    if (uda % 1ULL != 0ULL) return 53;
    if (uda / uda != 1ULL) return 54;
    if (uda % uda != 0ULL) return 55;

    /* Compound assignments: /= and %= */
    long long ca = 1000000000000LL;
    ca /= 3000LL;
    if (ca != 333333333LL) return 56;
    ca %= 1000LL;
    if (ca != 333LL) return 57;

    unsigned long long uca = 18446744073709551615ULL;
    uca /= 1000ULL;
    if (uca != 18446744073709551ULL) return 58;
    uca %= 1000ULL;
    if (uca != 551ULL) return 59;

    return 0; /* All passed */
}
