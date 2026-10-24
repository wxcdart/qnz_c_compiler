/* Test function definitions, calls, and prototypes with up to 32 parameters.
 *
 * Verifies SysV AMD64 / AAPCS64 / RISC-V calling conventions when functions
 * exceed register argument counts and require stack arguments (>6 on x86-64,
 * >8 on AArch64/RISC-V), especially when caller argument temporaries spill to
 * the frame and stack pointer shifts must be accounted for (zero_cc_frame_disp_at).
 */
#include <stdio.h>

int fn11(int p0, int p1, int p2, int p3, int p4, int p5,
         int p6, int p7, int p8, int p9, int p10) {
    return p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9 + p10;
}

int fn16(int p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7,
         int p8, int p9, int p10, int p11, int p12, int p13, int p14, int p15) {
    return p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 +
           p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15;
}

int fn32(int p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7,
         int p8, int p9, int p10, int p11, int p12, int p13, int p14, int p15,
         int p16, int p17, int p18, int p19, int p20, int p21, int p22, int p23,
         int p24, int p25, int p26, int p27, int p28, int p29, int p30, int p31) {
    int sum = 0;
    sum += p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7;
    sum += p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15;
    sum += p16 + p17 + p18 + p19 + p20 + p21 + p22 + p23;
    sum += p24 + p25 + p26 + p27 + p28 + p29 + p30 + p31;
    return sum;
}

int main(void) {
    int r11 = fn11(10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
    printf("fn11: %d\n", r11);

    int r16 = fn16(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    printf("fn16: %d\n", r16);

    int r32 = fn32(1, 1, 1, 1, 1, 1, 1, 1,
                   2, 2, 2, 2, 2, 2, 2, 2,
                   3, 3, 3, 3, 3, 3, 3, 3,
                   4, 4, 4, 4, 4, 4, 4, 4);
    printf("fn32: %d\n", r32);

    return 0;
}
