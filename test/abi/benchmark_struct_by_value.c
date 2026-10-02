#include <stdio.h>
#include <time.h>
#include <stdint.h>

// Simple 12-byte struct (3 ints)
typedef struct {
    int a;
    int b;
    int c;
} TwelveBytes;

// Function returning struct by value
static TwelveBytes make_struct(int i) {
    TwelveBytes s = {i, i+1, i+2};
    return s;
}

int main(void) {
    const long N = 10000000L; // 10 million iterations
    volatile int sink = 0; // prevent optimization
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (long i = 0; i < N; ++i) {
        TwelveBytes s = make_struct((int)i);
        sink += s.a + s.b + s.c;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    printf("Elapsed time: %.6f seconds, sink=%d\n", elapsed, sink);
    return 0;
}
