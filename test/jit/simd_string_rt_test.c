#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define GUARD_BYTE 0xEE
#define BUF_SIZE 512

static void fill_guard(uint8_t *buf, size_t size) {
    for (size_t i = 0; i < size; i++) buf[i] = GUARD_BYTE;
}

static int check_guard(const uint8_t *buf, size_t start, size_t end) {
    for (size_t i = start; i < end; i++) {
        if (buf[i] != GUARD_BYTE) {
            printf("Guard corruption at index %zu: expected 0x%02x, got 0x%02x\n",
                   i, GUARD_BYTE, buf[i]);
            return 0;
        }
    }
    return 1;
}

static void test_memcpy(void) {
    uint8_t src[BUF_SIZE];
    uint8_t dst[BUF_SIZE];

    for (size_t i = 0; i < BUF_SIZE; i++) {
        src[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }

    /* Test sizes 0 through 130 across source offsets 0..15 and dest offsets 0..15 */
    size_t sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 47, 48, 63, 64, 65, 127, 128, 129};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t s = 0; s < num_sizes; s++) {
        size_t len = sizes[s];
        for (size_t src_off = 0; src_off < 16; src_off++) {
            for (size_t dst_off = 0; dst_off < 16; dst_off++) {
                fill_guard(dst, BUF_SIZE);

                void *ret = memcpy(dst + dst_off + 32, src + src_off + 32, len);
                if (ret != dst + dst_off + 32) {
                    printf("memcpy return value mismatch\n");
                    exit(1);
                }

                if (!check_guard(dst, 0, dst_off + 32) ||
                    !check_guard(dst, dst_off + 32 + len, BUF_SIZE)) {
                    printf("memcpy guard zone check failed for len=%zu, src_off=%zu, dst_off=%zu\n",
                           len, src_off, dst_off);
                    exit(1);
                }

                for (size_t i = 0; i < len; i++) {
                    if (dst[dst_off + 32 + i] != src[src_off + 32 + i]) {
                        printf("memcpy data mismatch at byte %zu (len=%zu)\n", i, len);
                        exit(1);
                    }
                }
            }
        }
    }
    printf("memcpy: OK\n");
}

static void test_memset(void) {
    uint8_t buf[BUF_SIZE];
    uint8_t patterns[] = {0x00, 0xFF, 0x55, 0xAA, 0x7E, 0x81, 0x42};
    size_t num_patterns = sizeof(patterns) / sizeof(patterns[0]);

    size_t sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 47, 48, 63, 64, 65, 127, 128, 129};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t p = 0; p < num_patterns; p++) {
        uint8_t byte_val = patterns[p];
        for (size_t s = 0; s < num_sizes; s++) {
            size_t len = sizes[s];
            for (size_t off = 0; off < 16; off++) {
                fill_guard(buf, BUF_SIZE);

                void *ret = memset(buf + off + 32, byte_val, len);
                if (ret != buf + off + 32) {
                    printf("memset return value mismatch\n");
                    exit(1);
                }

                if (!check_guard(buf, 0, off + 32) ||
                    !check_guard(buf, off + 32 + len, BUF_SIZE)) {
                    printf("memset guard zone check failed for len=%zu, off=%zu\n", len, off);
                    exit(1);
                }

                for (size_t i = 0; i < len; i++) {
                    if (buf[off + 32 + i] != byte_val) {
                        printf("memset data mismatch at byte %zu: expected 0x%02x, got 0x%02x\n",
                               i, byte_val, buf[off + 32 + i]);
                        exit(1);
                    }
                }
            }
        }
    }
    printf("memset: OK\n");
}

static void test_memcmp(void) {
    uint8_t buf1[BUF_SIZE];
    uint8_t buf2[BUF_SIZE];

    for (size_t i = 0; i < BUF_SIZE; i++) {
        buf1[i] = (uint8_t)((i * 17 + 5) & 0xFF);
        buf2[i] = buf1[i];
    }

    size_t sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 64, 65, 128};
    size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t s = 0; s < num_sizes; s++) {
        size_t len = sizes[s];
        for (size_t off = 0; off < 16; off++) {
            /* Test equal */
            if (memcmp(buf1 + off + 32, buf2 + off + 32, len) != 0) {
                printf("memcmp equal check failed for len=%zu, off=%zu\n", len, off);
                exit(1);
            }

            if (len > 0) {
                /* Mismatch at first byte */
                buf1[off + 32] ^= 0x01;
                int res = memcmp(buf1 + off + 32, buf2 + off + 32, len);
                if (res == 0) {
                    printf("memcmp first byte mismatch not detected for len=%zu\n", len);
                    exit(1);
                }
                buf1[off + 32] ^= 0x01;

                /* Mismatch at last byte */
                buf1[off + 32 + len - 1] ^= 0x01;
                res = memcmp(buf1 + off + 32, buf2 + off + 32, len);
                if (res == 0) {
                    printf("memcmp last byte mismatch not detected for len=%zu\n", len);
                    exit(1);
                }
                buf1[off + 32 + len - 1] ^= 0x01;
            }
        }
    }
    printf("memcmp: OK\n");
}

static void test_strlen(void) {
    char str[BUF_SIZE];
    memset(str, 'A', sizeof(str));

    size_t lengths[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 47, 48, 63, 64, 65, 127, 128};
    size_t num_lengths = sizeof(lengths) / sizeof(lengths[0]);

    for (size_t l = 0; l < num_lengths; l++) {
        size_t len = lengths[l];
        for (size_t off = 0; off < 16; off++) {
            char *p = str + off + 32;
            /* Fill with non-zero characters, including high-bit characters */
            for (size_t i = 0; i < len; i++) {
                p[i] = (char)((i % 2 == 0) ? ('B' + (i % 20)) : (char)(0x80 | (i & 0x7F)));
                if (p[i] == '\0') p[i] = 'Z';
            }
            p[len] = '\0';

            size_t calculated = strlen(p);
            if (calculated != len) {
                printf("strlen mismatch: expected %zu, got %zu (off=%zu)\n", len, calculated, off);
                exit(1);
            }
        }
    }
    printf("strlen: OK\n");
}

static void test_memmove(void) {
    uint8_t buf[BUF_SIZE];

    for (size_t i = 0; i < BUF_SIZE; i++) buf[i] = (uint8_t)(i & 0xFF);

    /* Forward overlap: src < dst */
    size_t len = 64;
    size_t src_off = 32;
    size_t dst_off = 48; /* overlap by 48 bytes */
    memmove(buf + dst_off, buf + src_off, len);
    for (size_t i = 0; i < len; i++) {
        if (buf[dst_off + i] != (uint8_t)((src_off + i) & 0xFF)) {
            printf("memmove forward overlap mismatch at %zu\n", i);
            exit(1);
        }
    }

    /* Backward overlap: dst < src */
    for (size_t i = 0; i < BUF_SIZE; i++) buf[i] = (uint8_t)(i & 0xFF);
    src_off = 48;
    dst_off = 32;
    memmove(buf + dst_off, buf + src_off, len);
    for (size_t i = 0; i < len; i++) {
        if (buf[dst_off + i] != (uint8_t)((src_off + i) & 0xFF)) {
            printf("memmove backward overlap mismatch at %zu\n", i);
            exit(1);
        }
    }
    printf("memmove: OK\n");
}

int main(void) {
    test_memcpy();
    test_memset();
    test_memcmp();
    test_strlen();
    test_memmove();
    printf("ALL SIMD STRING RT TESTS PASSED!\n");
    return 0;
}
