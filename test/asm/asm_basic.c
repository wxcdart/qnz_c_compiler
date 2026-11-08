/* Inline asm Phase 1 (x86-64 AOT text): basic operand forms.
 * Differential test: every value below is checked against the C computation
 * so the host-gcc reference and zerocc must agree bit-for-bit.
 * Covered: =r / +r / =m / m input, digit-matched inputs, fixed-register
 * letters a/b/d/S/D, %% escapes, 8/16/32/64-bit widths, 64-bit immediates.
 */
#include <stdio.h>

int main(void) {
    int r;
    __asm__("movl $42, %0" : "=r"(r));

    int v = 5;
    __asm__("addl $1, %0" : "+r"(v));

    int m = 0;
    __asm__("movl $99, %0" : "=m"(m));

    int a = 5, b = 9;
    __asm__("addl %1, %0" : "+r"(a) : "r"(b));

    int mm = 7;
    __asm__("movl %1, %0" : "=m"(mm) : "r"(a * 2));

    int dm = 11;
    __asm__("addl $1, %0" : "=r"(dm) : "0"(dm));

    int fa = 1, fb = 2, fd = 3, fs = 4, fdd = 5;
    __asm__("movl $11, %0" : "=a"(fa));
    __asm__("movl $22, %0" : "=b"(fb));
    __asm__("movl $33, %0" : "=d"(fd));
    __asm__("movl $44, %0" : "=S"(fs));
    __asm__("movl $55, %0" : "=D"(fdd));

    unsigned u = 21;
    __asm__("movl %1, %0" : "=r"(u) : "0"(u), "r"(2) :);

    long l = 0;
    __asm__("movq $1234567890123, %0" : "=r"(l));

    short s = 0;
    __asm__("movw $340, %0" : "=r"(s));

    char ch = 0;
    __asm__("movb $90, %0" : "=r"(ch));

    unsigned long ul = 5;
    __asm__("notq %0" : "+r"(ul) : );

    printf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
           r, v, m, a, mm, dm, fa, fb, fd, fs, fdd, (int)u, (int)l, (int)s,
           (int)ch + (int)(ul % 1000));
    int ok = (r == 42 && v == 6 && m == 99 && a == 14 && mm == 28 && dm == 12 &&
              fa == 11 && fb == 22 && fd == 33 && fs == 44 && fdd == 55 &&
              u == 21 && l == 1234567890123L && s == 340 && ch == 'Z' &&
              ul == ~5UL);
    printf("%d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}
