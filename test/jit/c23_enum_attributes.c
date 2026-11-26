/* C23 Standard Attributes and Fixed Underlying Enum Types (Sprint 254) */
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

[[nodiscard]] int add2(int a, int b) {
    return a + b;
}

[[nodiscard("reason string")]] int mul2(int a, int b) {
    return a * b;
}

[[deprecated]] int old_sub(int a, int b) {
    return a - b;
}

[[deprecated("use sub instead")]] int legacy_calc(int x) {
    return x * 3;
}

[[maybe_unused]] static int g_unused = 999;
[[deprecated]] static int g_deprecated_var = 1234;

int test_params([[maybe_unused]] int a, [[deprecated]] int b) {
    return a + b;
}

enum [[maybe_unused]] EColor : uint8_t {
    COLOR_RED [[deprecated]] = 1,
    COLOR_GREEN [[maybe_unused]] = 2,
    COLOR_BLUE [[nodiscard]] = 3
};

enum EWidth : uint16_t {
    WIDTH_NARROW = 100,
    WIDTH_WIDE = 50000
};

enum EBig : uint64_t {
    BIG_MASK = 0x8000000000000000ULL,
    BIG_VAL  = 0x123456789ABCDEF0ULL
};

enum EFwd : uint8_t;
enum EFwd : uint8_t {
    EFWD_A = 10,
    EFWD_B = 20
};

typedef enum : uint8_t {
    ANON_X = 7,
    ANON_Y = 8
} anon_enum_u8_t;

int test_switch_fallthrough(int code) {
    int accum = 0;
    switch (code) {
    case 1:
        accum += 100;
        [[fallthrough]];
    case 2:
        accum += 50;
        [[fallthrough]];
    case 3:
        accum += 10;
        break;
    default:
        accum = -1;
        break;
    }
    return accum;
}

int main(void) {
    static_assert(sizeof(enum EColor) == 1, "EColor size == 1");
    static_assert(sizeof(enum EWidth) == 2, "EWidth size == 2");
    static_assert(sizeof(enum EBig) == 8, "EBig size == 8");
    static_assert(sizeof(enum EFwd) == 1, "EFwd size == 1");
    static_assert(sizeof(anon_enum_u8_t) == 1, "anon_enum_u8_t size == 1");

    [[maybe_unused]] int local_u = 42;
    [[deprecated]] int local_d = 77;
    int var1 [[maybe_unused]] = 10, var2 [[deprecated]] = 20;

    if (add2(3, 4) != 7) return 1;
    if (mul2(5, 6) != 30) return 2;
    if (old_sub(10, 4) != 6) return 3;
    if (legacy_calc(4) != 12) return 4;
    if (test_params(var1, var2) != 30) return 5;

    if (COLOR_RED != 1 || COLOR_GREEN != 2 || COLOR_BLUE != 3) return 6;
    if (WIDTH_NARROW != 100 || WIDTH_WIDE != 50000) return 7;
    if (BIG_VAL != 0x123456789ABCDEF0ULL) return 8;
    if (EFWD_A != 10 || EFWD_B != 20) return 9;
    if (ANON_X != 7 || ANON_Y != 8) return 10;

    if (test_switch_fallthrough(1) != 160) return 11;
    if (test_switch_fallthrough(2) != 60) return 12;
    if (test_switch_fallthrough(3) != 10) return 13;
    if (test_switch_fallthrough(99) != -1) return 14;

    return 0;
}
