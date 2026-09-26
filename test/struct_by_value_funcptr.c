#include <stdio.h>

typedef struct { int x; int y; } Pair;

// Function returning a struct
Pair make_pair(int a, int b) {
    Pair p = { a, b };
    return p;
}

// Function pointer type returning Pair
typedef Pair (*pair_func_t)(int, int);

int main(void) {
    // Assign function to pointer
    pair_func_t f = make_pair;
    Pair p = f(5, 7);
    if (p.x + p.y != 12) {
        fprintf(stderr, "FAIL funcptr %d %d\n", p.x, p.y);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
