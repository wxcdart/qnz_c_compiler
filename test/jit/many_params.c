/* Test function definitions, calls, and prototypes with up to 128 parameters.
 *
 * Verifies SysV AMD64 / AAPCS64 / RISC-V calling conventions when functions
 * exceed register argument counts and require stack arguments (>6 on x86-64,
 * >8 on AArch64/RISC-V), especially when caller argument temporaries spill to
 * the frame and stack pointer shifts must be accounted for (zero_cc_frame_disp_at).
 * Also tests compliance with C99/C11 translation limits (at least 127 parameters).
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

int fn128(int p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, int p10, int p11, int p12, int p13, int p14, int p15, int p16, int p17, int p18, int p19, int p20, int p21, int p22, int p23, int p24, int p25, int p26, int p27, int p28, int p29, int p30, int p31, int p32, int p33, int p34, int p35, int p36, int p37, int p38, int p39, int p40, int p41, int p42, int p43, int p44, int p45, int p46, int p47, int p48, int p49, int p50, int p51, int p52, int p53, int p54, int p55, int p56, int p57, int p58, int p59, int p60, int p61, int p62, int p63, int p64, int p65, int p66, int p67, int p68, int p69, int p70, int p71, int p72, int p73, int p74, int p75, int p76, int p77, int p78, int p79, int p80, int p81, int p82, int p83, int p84, int p85, int p86, int p87, int p88, int p89, int p90, int p91, int p92, int p93, int p94, int p95, int p96, int p97, int p98, int p99, int p100, int p101, int p102, int p103, int p104, int p105, int p106, int p107, int p108, int p109, int p110, int p111, int p112, int p113, int p114, int p115, int p116, int p117, int p118, int p119, int p120, int p121, int p122, int p123, int p124, int p125, int p126, int p127) {
    int sum = 0;
    sum += p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9 + p10 + p11 + p12 + p13 + p14 + p15;
    sum += p16 + p17 + p18 + p19 + p20 + p21 + p22 + p23 + p24 + p25 + p26 + p27 + p28 + p29 + p30 + p31;
    sum += p32 + p33 + p34 + p35 + p36 + p37 + p38 + p39 + p40 + p41 + p42 + p43 + p44 + p45 + p46 + p47;
    sum += p48 + p49 + p50 + p51 + p52 + p53 + p54 + p55 + p56 + p57 + p58 + p59 + p60 + p61 + p62 + p63;
    sum += p64 + p65 + p66 + p67 + p68 + p69 + p70 + p71 + p72 + p73 + p74 + p75 + p76 + p77 + p78 + p79;
    sum += p80 + p81 + p82 + p83 + p84 + p85 + p86 + p87 + p88 + p89 + p90 + p91 + p92 + p93 + p94 + p95;
    sum += p96 + p97 + p98 + p99 + p100 + p101 + p102 + p103 + p104 + p105 + p106 + p107 + p108 + p109 + p110 + p111;
    sum += p112 + p113 + p114 + p115 + p116 + p117 + p118 + p119 + p120 + p121 + p122 + p123 + p124 + p125 + p126 + p127;
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

    int r128 = fn128(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128);
    printf("fn128: %d\n", r128);

    if (r11 != 165 || r16 != 136 || r32 != 80 || r128 != 8256) {
        return 1;
    }
    return 0;
}
