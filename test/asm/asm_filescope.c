/* Inline asm Phase 4: file-scope asm statements.
 * Top-level __asm__("...") defines functions and data.
 * Tested across text AOT (-S), JIT (--run), and raw AOT.
 */
#include <stdio.h>

__asm__(
    ".text\n"
    ".globl add_ten\n"
    ".type add_ten, @function\n"
    "add_ten:\n"
    "  movl %edi, %eax\n"
    "  addl $10, %eax\n"
    "  ret\n"
);

__asm__(
    ".globl mul_two\n"
    "mul_two:\n"
    "  movl %edi, %eax\n"
    "  addl %eax, %eax\n"
    "  ret\n"
);

int add_ten(int x);
int mul_two(int x);

int main(void) {
    int a = add_ten(32);
    int b = mul_two(21);
    printf("%d %d\n", a, b);
    int ok = (a == 42 && b == 42);
    printf("%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
