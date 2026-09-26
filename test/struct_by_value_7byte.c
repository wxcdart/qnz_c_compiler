#include <stdio.h>

// 7-byte struct (char[7])
typedef struct { char a[7]; } SevenByte;
static SevenByte make_seven(void) { SevenByte s = { "ABCDEFG" }; return s; }
int main(void) {
    volatile SevenByte s = make_seven();
    (void)s;
    return 0;
}
