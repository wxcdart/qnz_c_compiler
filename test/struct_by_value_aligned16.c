#include <stdio.h>

/* 16-byte aligned struct with explicit alignment */
typedef struct __attribute__((aligned(16))) {
    long a;   // 8 bytes
    int  b;   // 4 bytes
    short c;  // 2 bytes
    char d;   // 1 byte (padding to 16)
} Aligned16;

static Aligned16 make_aligned(void) {
    Aligned16 s = { 0x1122334455667788, 0xAABBCCDD, 0xEEFF, 0x12 };
    return s;
}

int main(void) {
    volatile Aligned16 s = make_aligned();
    (void)s;
    return 0;
}
