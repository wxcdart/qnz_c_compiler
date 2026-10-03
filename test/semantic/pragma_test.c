/* Test pragma support: pack, message, GCC diagnostic */
#include <stdio.h>

/* Test #pragma message */
#pragma message("This is a test message")

/* Test #pragma pack */
#pragma pack(push, 1)
struct PackedStruct {
    char a;
    int b;
};
#pragma pack(pop)

/* Test #pragma pack with value */
#pragma pack(push, 2)
struct PackedStruct2 {
    char a;
    int b;
};
#pragma pack(pop)

/* Test #pragma GCC diagnostic push/pop */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
int unused_var;  /* Should not warn */
#pragma GCC diagnostic pop

int main(void) {
    struct PackedStruct s1;
    struct PackedStruct2 s2;
    
    printf("sizeof(PackedStruct) = %zu (expected %zu)\n", sizeof(s1), 5);
    printf("sizeof(PackedStruct2) = %zu (expected 6)\n", sizeof(s2));
    
    return 0;
}
