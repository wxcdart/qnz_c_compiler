#include <stdio.h>

// 2-byte struct (short)
typedef struct { short a; } TwoByte;
static TwoByte make_two(void) { TwoByte s = { 0x1234 }; return s; }
int main(void) { TwoByte s = make_two(); (void)s; return 0; }
