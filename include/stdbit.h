#ifndef _STDBIT_H
#define _STDBIT_H 1

#define __STDC_VERSION_STDBIT_H__ 202311L

#define __STDC_ENDIAN_LITTLE__ 1234
#define __STDC_ENDIAN_BIG__    4321
#define __STDC_ENDIAN_NATIVE__ 1234

#define __STDC_BIT_WIDTH_INT   ((unsigned int)(sizeof(unsigned int) * 8))
#define __STDC_BIT_WIDTH_LONG  ((unsigned int)(sizeof(unsigned long) * 8))

static inline unsigned int stdc_leading_zeros_uc(unsigned char __v) {
    return __v == 0 ? 8 : (unsigned int)(__builtin_clz((unsigned int)__v) - (__STDC_BIT_WIDTH_INT - 8));
}
static inline unsigned int stdc_leading_zeros_us(unsigned short __v) {
    return __v == 0 ? 16 : (unsigned int)(__builtin_clz((unsigned int)__v) - (__STDC_BIT_WIDTH_INT - 16));
}
static inline unsigned int stdc_leading_zeros_ui(unsigned int __v) {
    return __v == 0 ? 32 : (unsigned int)__builtin_clz(__v);
}
static inline unsigned int stdc_leading_zeros_ul(unsigned long __v) {
    if (__v == 0) return __STDC_BIT_WIDTH_LONG;
    return sizeof(unsigned long) == 8 ? (unsigned int)__builtin_clzll((unsigned long long)__v) : (unsigned int)__builtin_clz((unsigned int)__v);
}
static inline unsigned int stdc_leading_zeros_ull(unsigned long long __v) {
    return __v == 0 ? 64 : (unsigned int)__builtin_clzll(__v);
}

static inline unsigned int stdc_trailing_zeros_uc(unsigned char __v) {
    return __v == 0 ? 8 : (unsigned int)__builtin_ctz((unsigned int)__v);
}
static inline unsigned int stdc_trailing_zeros_us(unsigned short __v) {
    return __v == 0 ? 16 : (unsigned int)__builtin_ctz((unsigned int)__v);
}
static inline unsigned int stdc_trailing_zeros_ui(unsigned int __v) {
    return __v == 0 ? 32 : (unsigned int)__builtin_ctz(__v);
}
static inline unsigned int stdc_trailing_zeros_ul(unsigned long __v) {
    if (__v == 0) return __STDC_BIT_WIDTH_LONG;
    return sizeof(unsigned long) == 8 ? (unsigned int)__builtin_ctzll((unsigned long long)__v) : (unsigned int)__builtin_ctz((unsigned int)__v);
}
static inline unsigned int stdc_trailing_zeros_ull(unsigned long long __v) {
    return __v == 0 ? 64 : (unsigned int)__builtin_ctzll(__v);
}

static inline unsigned int stdc_count_ones_uc(unsigned char __v) {
    return (unsigned int)__builtin_popcount((unsigned int)__v);
}
static inline unsigned int stdc_count_ones_us(unsigned short __v) {
    return (unsigned int)__builtin_popcount((unsigned int)__v);
}
static inline unsigned int stdc_count_ones_ui(unsigned int __v) {
    return (unsigned int)__builtin_popcount(__v);
}
static inline unsigned int stdc_count_ones_ul(unsigned long __v) {
    return sizeof(unsigned long) == 8 ? (unsigned int)__builtin_popcountll((unsigned long long)__v) : (unsigned int)__builtin_popcount((unsigned int)__v);
}
static inline unsigned int stdc_count_ones_ull(unsigned long long __v) {
    return (unsigned int)__builtin_popcountll(__v);
}

static inline unsigned int stdc_count_zeros_uc(unsigned char __v) { return 8 - stdc_count_ones_uc(__v); }
static inline unsigned int stdc_count_zeros_us(unsigned short __v) { return 16 - stdc_count_ones_us(__v); }
static inline unsigned int stdc_count_zeros_ui(unsigned int __v) { return 32 - stdc_count_ones_ui(__v); }
static inline unsigned int stdc_count_zeros_ul(unsigned long __v) { return __STDC_BIT_WIDTH_LONG - stdc_count_ones_ul(__v); }
static inline unsigned int stdc_count_zeros_ull(unsigned long long __v) { return 64 - stdc_count_ones_ull(__v); }

static inline unsigned int stdc_leading_ones_uc(unsigned char __v) { return stdc_leading_zeros_uc((unsigned char)~__v); }
static inline unsigned int stdc_leading_ones_us(unsigned short __v) { return stdc_leading_zeros_us((unsigned short)~__v); }
static inline unsigned int stdc_leading_ones_ui(unsigned int __v) { return stdc_leading_zeros_ui(~__v); }
static inline unsigned int stdc_leading_ones_ul(unsigned long __v) { return stdc_leading_zeros_ul(~__v); }
static inline unsigned int stdc_leading_ones_ull(unsigned long long __v) { return stdc_leading_zeros_ull(~__v); }

static inline unsigned int stdc_trailing_ones_uc(unsigned char __v) { return stdc_trailing_zeros_uc((unsigned char)~__v); }
static inline unsigned int stdc_trailing_ones_us(unsigned short __v) { return stdc_trailing_zeros_us((unsigned short)~__v); }
static inline unsigned int stdc_trailing_ones_ui(unsigned int __v) { return stdc_trailing_zeros_ui(~__v); }
static inline unsigned int stdc_trailing_ones_ul(unsigned long __v) { return stdc_trailing_zeros_ul(~__v); }
static inline unsigned int stdc_trailing_ones_ull(unsigned long long __v) { return stdc_trailing_zeros_ull(~__v); }

static inline unsigned int stdc_first_leading_zero_uc(unsigned char __v) {
    return __v == (unsigned char)0xFF ? 0 : stdc_leading_ones_uc(__v) + 1;
}
static inline unsigned int stdc_first_leading_zero_us(unsigned short __v) {
    return __v == (unsigned short)0xFFFF ? 0 : stdc_leading_ones_us(__v) + 1;
}
static inline unsigned int stdc_first_leading_zero_ui(unsigned int __v) {
    return __v == (unsigned int)0xFFFFFFFFU ? 0 : stdc_leading_ones_ui(__v) + 1;
}
static inline unsigned int stdc_first_leading_zero_ul(unsigned long __v) {
    return __v == (unsigned long)~0UL ? 0 : stdc_leading_ones_ul(__v) + 1;
}
static inline unsigned int stdc_first_leading_zero_ull(unsigned long long __v) {
    return __v == (unsigned long long)~0ULL ? 0 : stdc_leading_ones_ull(__v) + 1;
}

static inline unsigned int stdc_first_leading_one_uc(unsigned char __v) {
    return __v == 0 ? 0 : stdc_leading_zeros_uc(__v) + 1;
}
static inline unsigned int stdc_first_leading_one_us(unsigned short __v) {
    return __v == 0 ? 0 : stdc_leading_zeros_us(__v) + 1;
}
static inline unsigned int stdc_first_leading_one_ui(unsigned int __v) {
    return __v == 0 ? 0 : stdc_leading_zeros_ui(__v) + 1;
}
static inline unsigned int stdc_first_leading_one_ul(unsigned long __v) {
    return __v == 0 ? 0 : stdc_leading_zeros_ul(__v) + 1;
}
static inline unsigned int stdc_first_leading_one_ull(unsigned long long __v) {
    return __v == 0 ? 0 : stdc_leading_zeros_ull(__v) + 1;
}

static inline unsigned int stdc_first_trailing_zero_uc(unsigned char __v) {
    return __v == (unsigned char)0xFF ? 0 : stdc_trailing_ones_uc(__v) + 1;
}
static inline unsigned int stdc_first_trailing_zero_us(unsigned short __v) {
    return __v == (unsigned short)0xFFFF ? 0 : stdc_trailing_ones_us(__v) + 1;
}
static inline unsigned int stdc_first_trailing_zero_ui(unsigned int __v) {
    return __v == (unsigned int)0xFFFFFFFFU ? 0 : stdc_trailing_ones_ui(__v) + 1;
}
static inline unsigned int stdc_first_trailing_zero_ul(unsigned long __v) {
    return __v == (unsigned long)~0UL ? 0 : stdc_trailing_ones_ul(__v) + 1;
}
static inline unsigned int stdc_first_trailing_zero_ull(unsigned long long __v) {
    return __v == (unsigned long long)~0ULL ? 0 : stdc_trailing_ones_ull(__v) + 1;
}

static inline unsigned int stdc_first_trailing_one_uc(unsigned char __v) {
    return __v == 0 ? 0 : stdc_trailing_zeros_uc(__v) + 1;
}
static inline unsigned int stdc_first_trailing_one_us(unsigned short __v) {
    return __v == 0 ? 0 : stdc_trailing_zeros_us(__v) + 1;
}
static inline unsigned int stdc_first_trailing_one_ui(unsigned int __v) {
    return __v == 0 ? 0 : stdc_trailing_zeros_ui(__v) + 1;
}
static inline unsigned int stdc_first_trailing_one_ul(unsigned long __v) {
    return __v == 0 ? 0 : stdc_trailing_zeros_ul(__v) + 1;
}
static inline unsigned int stdc_first_trailing_one_ull(unsigned long long __v) {
    return __v == 0 ? 0 : stdc_trailing_zeros_ull(__v) + 1;
}

static inline _Bool stdc_has_single_bit_uc(unsigned char __v) {
    return (__v != 0) && ((__v & (__v - 1)) == 0);
}
static inline _Bool stdc_has_single_bit_us(unsigned short __v) {
    return (__v != 0) && ((__v & (__v - 1)) == 0);
}
static inline _Bool stdc_has_single_bit_ui(unsigned int __v) {
    return (__v != 0) && ((__v & (__v - 1)) == 0);
}
static inline _Bool stdc_has_single_bit_ul(unsigned long __v) {
    return (__v != 0) && ((__v & (__v - 1)) == 0);
}
static inline _Bool stdc_has_single_bit_ull(unsigned long long __v) {
    return (__v != 0) && ((__v & (__v - 1)) == 0);
}

static inline unsigned int stdc_bit_width_uc(unsigned char __v) {
    return __v == 0 ? 0 : 8 - stdc_leading_zeros_uc(__v);
}
static inline unsigned int stdc_bit_width_us(unsigned short __v) {
    return __v == 0 ? 0 : 16 - stdc_leading_zeros_us(__v);
}
static inline unsigned int stdc_bit_width_ui(unsigned int __v) {
    return __v == 0 ? 0 : 32 - stdc_leading_zeros_ui(__v);
}
static inline unsigned int stdc_bit_width_ul(unsigned long __v) {
    return __v == 0 ? 0 : __STDC_BIT_WIDTH_LONG - stdc_leading_zeros_ul(__v);
}
static inline unsigned int stdc_bit_width_ull(unsigned long long __v) {
    return __v == 0 ? 0 : 64 - stdc_leading_zeros_ull(__v);
}

static inline unsigned char stdc_bit_floor_uc(unsigned char __v) {
    return __v == 0 ? (unsigned char)0 : (unsigned char)(1U << (8 - 1 - stdc_leading_zeros_uc(__v)));
}
static inline unsigned short stdc_bit_floor_us(unsigned short __v) {
    return __v == 0 ? (unsigned short)0 : (unsigned short)(1U << (16 - 1 - stdc_leading_zeros_us(__v)));
}
static inline unsigned int stdc_bit_floor_ui(unsigned int __v) {
    return __v == 0 ? 0U : (1U << (32 - 1 - stdc_leading_zeros_ui(__v)));
}
static inline unsigned long stdc_bit_floor_ul(unsigned long __v) {
    return __v == 0 ? 0UL : (1UL << (__STDC_BIT_WIDTH_LONG - 1 - stdc_leading_zeros_ul(__v)));
}
static inline unsigned long long stdc_bit_floor_ull(unsigned long long __v) {
    return __v == 0 ? 0ULL : (1ULL << (64 - 1 - stdc_leading_zeros_ull(__v)));
}

static inline unsigned char stdc_bit_ceil_uc(unsigned char __v) {
    if (__v <= 1) return (unsigned char)1;
    unsigned int __lz = stdc_leading_zeros_uc((unsigned char)(__v - 1));
    if (__lz == 0) return (unsigned char)0;
    return (unsigned char)(1U << (8 - __lz));
}
static inline unsigned short stdc_bit_ceil_us(unsigned short __v) {
    if (__v <= 1) return (unsigned short)1;
    unsigned int __lz = stdc_leading_zeros_us((unsigned short)(__v - 1));
    if (__lz == 0) return (unsigned short)0;
    return (unsigned short)(1U << (16 - __lz));
}
static inline unsigned int stdc_bit_ceil_ui(unsigned int __v) {
    if (__v <= 1) return 1U;
    unsigned int __lz = stdc_leading_zeros_ui(__v - 1);
    if (__lz == 0) return 0U;
    return 1U << (32 - __lz);
}
static inline unsigned long stdc_bit_ceil_ul(unsigned long __v) {
    if (__v <= 1) return 1UL;
    unsigned int __lz = stdc_leading_zeros_ul(__v - 1);
    if (__lz == 0) return 0UL;
    return 1UL << (__STDC_BIT_WIDTH_LONG - __lz);
}
static inline unsigned long long stdc_bit_ceil_ull(unsigned long long __v) {
    if (__v <= 1) return 1ULL;
    unsigned int __lz = stdc_leading_zeros_ull(__v - 1);
    if (__lz == 0) return 0ULL;
    return 1ULL << (64 - __lz);
}

#define stdc_leading_zeros(v) _Generic((v), \
    unsigned char: stdc_leading_zeros_uc(v), \
    unsigned short: stdc_leading_zeros_us(v), \
    unsigned int: stdc_leading_zeros_ui(v), \
    unsigned long: stdc_leading_zeros_ul(v), \
    unsigned long long: stdc_leading_zeros_ull(v))

#define stdc_leading_ones(v) _Generic((v), \
    unsigned char: stdc_leading_ones_uc(v), \
    unsigned short: stdc_leading_ones_us(v), \
    unsigned int: stdc_leading_ones_ui(v), \
    unsigned long: stdc_leading_ones_ul(v), \
    unsigned long long: stdc_leading_ones_ull(v))

#define stdc_trailing_zeros(v) _Generic((v), \
    unsigned char: stdc_trailing_zeros_uc(v), \
    unsigned short: stdc_trailing_zeros_us(v), \
    unsigned int: stdc_trailing_zeros_ui(v), \
    unsigned long: stdc_trailing_zeros_ul(v), \
    unsigned long long: stdc_trailing_zeros_ull(v))

#define stdc_trailing_ones(v) _Generic((v), \
    unsigned char: stdc_trailing_ones_uc(v), \
    unsigned short: stdc_trailing_ones_us(v), \
    unsigned int: stdc_trailing_ones_ui(v), \
    unsigned long: stdc_trailing_ones_ul(v), \
    unsigned long long: stdc_trailing_ones_ull(v))

#define stdc_first_leading_zero(v) _Generic((v), \
    unsigned char: stdc_first_leading_zero_uc(v), \
    unsigned short: stdc_first_leading_zero_us(v), \
    unsigned int: stdc_first_leading_zero_ui(v), \
    unsigned long: stdc_first_leading_zero_ul(v), \
    unsigned long long: stdc_first_leading_zero_ull(v))

#define stdc_first_leading_one(v) _Generic((v), \
    unsigned char: stdc_first_leading_one_uc(v), \
    unsigned short: stdc_first_leading_one_us(v), \
    unsigned int: stdc_first_leading_one_ui(v), \
    unsigned long: stdc_first_leading_one_ul(v), \
    unsigned long long: stdc_first_leading_one_ull(v))

#define stdc_first_trailing_zero(v) _Generic((v), \
    unsigned char: stdc_first_trailing_zero_uc(v), \
    unsigned short: stdc_first_trailing_zero_us(v), \
    unsigned int: stdc_first_trailing_zero_ui(v), \
    unsigned long: stdc_first_trailing_zero_ul(v), \
    unsigned long long: stdc_first_trailing_zero_ull(v))

#define stdc_first_trailing_one(v) _Generic((v), \
    unsigned char: stdc_first_trailing_one_uc(v), \
    unsigned short: stdc_first_trailing_one_us(v), \
    unsigned int: stdc_first_trailing_one_ui(v), \
    unsigned long: stdc_first_trailing_one_ul(v), \
    unsigned long long: stdc_first_trailing_one_ull(v))

#define stdc_count_zeros(v) _Generic((v), \
    unsigned char: stdc_count_zeros_uc(v), \
    unsigned short: stdc_count_zeros_us(v), \
    unsigned int: stdc_count_zeros_ui(v), \
    unsigned long: stdc_count_zeros_ul(v), \
    unsigned long long: stdc_count_zeros_ull(v))

#define stdc_count_ones(v) _Generic((v), \
    unsigned char: stdc_count_ones_uc(v), \
    unsigned short: stdc_count_ones_us(v), \
    unsigned int: stdc_count_ones_ui(v), \
    unsigned long: stdc_count_ones_ul(v), \
    unsigned long long: stdc_count_ones_ull(v))

#define stdc_has_single_bit(v) _Generic((v), \
    unsigned char: stdc_has_single_bit_uc(v), \
    unsigned short: stdc_has_single_bit_us(v), \
    unsigned int: stdc_has_single_bit_ui(v), \
    unsigned long: stdc_has_single_bit_ul(v), \
    unsigned long long: stdc_has_single_bit_ull(v))

#define stdc_bit_width(v) _Generic((v), \
    unsigned char: stdc_bit_width_uc(v), \
    unsigned short: stdc_bit_width_us(v), \
    unsigned int: stdc_bit_width_ui(v), \
    unsigned long: stdc_bit_width_ul(v), \
    unsigned long long: stdc_bit_width_ull(v))

#define stdc_bit_floor(v) _Generic((v), \
    unsigned char: stdc_bit_floor_uc(v), \
    unsigned short: stdc_bit_floor_us(v), \
    unsigned int: stdc_bit_floor_ui(v), \
    unsigned long: stdc_bit_floor_ul(v), \
    unsigned long long: stdc_bit_floor_ull(v))

#define stdc_bit_ceil(v) _Generic((v), \
    unsigned char: stdc_bit_ceil_uc(v), \
    unsigned short: stdc_bit_ceil_us(v), \
    unsigned int: stdc_bit_ceil_ui(v), \
    unsigned long: stdc_bit_ceil_ul(v), \
    unsigned long long: stdc_bit_ceil_ull(v))

#endif /* _STDBIT_H */
