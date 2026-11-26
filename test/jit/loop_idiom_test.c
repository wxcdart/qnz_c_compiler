#include <stdio.h>
#include <string.h>
#include <assert.h>

void zero_bytes(char *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i] = 0;
    }
}

void zero_ints(int *p, int n) {
    for (int i = 0; i < n; i++) {
        p[i] = 0;
    }
}

void fill_bytes(char *p, int n, char val) {
    for (int i = 0; i < n; i++) {
        p[i] = val;
    }
}

void copy_bytes(char *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void copy_ints(int *dst, const int *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

void copy_doubles(double *dst, const double *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

int count_after_loop(int *p, int n) {
    int i;
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
    return i;
}

int main(void) {
    /* 1. Zero bytes */
    char buf1[64];
    memset(buf1, 0xAA, sizeof(buf1));
    zero_bytes(buf1, 64);
    for (int i = 0; i < 64; i++) {
        assert(buf1[i] == 0);
    }

    /* 2. Zero ints */
    int buf2[32];
    for (int i = 0; i < 32; i++) buf2[i] = 123456;
    zero_ints(buf2, 32);
    for (int i = 0; i < 32; i++) {
        assert(buf2[i] == 0);
    }

    /* 3. Fill bytes */
    char buf3[50];
    fill_bytes(buf3, 50, 0x42);
    for (int i = 0; i < 50; i++) {
        assert(buf3[i] == 0x42);
    }

    /* 4. Copy bytes */
    char src4[40], dst4[40];
    for (int i = 0; i < 40; i++) src4[i] = (char)(i + 1);
    memset(dst4, 0, sizeof(dst4));
    copy_bytes(dst4, src4, 40);
    for (int i = 0; i < 40; i++) {
        assert(dst4[i] == (char)(i + 1));
    }

    /* 5. Copy ints */
    int src5[25], dst5[25];
    for (int i = 0; i < 25; i++) src5[i] = (i + 1) * 100;
    memset(dst5, 0, sizeof(dst5));
    copy_ints(dst5, src5, 25);
    for (int i = 0; i < 25; i++) {
        assert(dst5[i] == (i + 1) * 100);
    }

    /* 6. Copy doubles */
    double src6[20], dst6[20];
    for (int i = 0; i < 20; i++) src6[i] = (double)i * 1.5;
    memset(dst6, 0, sizeof(dst6));
    copy_doubles(dst6, src6, 20);
    for (int i = 0; i < 20; i++) {
        assert(dst6[i] == (double)i * 1.5);
    }

    /* 7. Zero-trip guard: should not crash or modify buffer */
    char guard_buf[10];
    memset(guard_buf, 0x77, sizeof(guard_buf));
    zero_bytes(guard_buf, 0);
    assert(guard_buf[0] == 0x77);
    zero_bytes(guard_buf, -5);
    assert(guard_buf[0] == 0x77);

    /* 8. Counter after loop */
    int buf8[16];
    int c = count_after_loop(buf8, 16);
    assert(c == 16);

    printf("All Loop Idiom Recognition tests PASSED\n");
    return 0;
}
