/* Struct by Value Test */
#include <assert.h>

struct Pair { int x; int y; };
struct Triple { int a; int b; int c; };

/* Pass struct by value - SysV: RDI, RSI for first two 8-byte chunks */
int take_pair(struct Pair p) {
    return p.x + p.y;
}

/* Return struct by value - SysV: RAX, RDX for two 8-byte chunks */
struct Pair make_pair(int a, int b) {
    struct Pair p = {a, b};
    return p;
}

/* Larger struct - passed/returned via hidden pointer */
struct Triple make_triple(int a, int b, int c) {
    struct Triple t = {a, b, c};
    return t;
}

int take_triple(struct Triple t) {
    return t.a + t.b + t.c;
}

int main(void) {
    /* Pass by value */
    assert(take_pair((struct Pair){10, 20}) == 30);
    
    /* Return by value */
    struct Pair p = make_pair(5, 7);
    assert(p.x == 5 && p.y == 7);
    
    /* Larger struct (3 ints = 12 bytes > 16 bytes? No, 12 < 16) */
    struct Triple t = make_triple(1, 2, 3);
    assert(t.a == 1 && t.b == 2 && t.c == 3);
    
    /* Pass larger struct by value */
    assert(take_triple((struct Triple){10, 20, 30}) == 60);
    
    return 0;
}