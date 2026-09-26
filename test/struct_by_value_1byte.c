#include <stdio.h>

// 1-byte struct (char)
typedef struct { char a; } OneByte;
static OneByte make_one(void) { OneByte s = { 'A' }; return s; }
int main(void) { OneByte s = make_one(); (void)s; return 0; }
