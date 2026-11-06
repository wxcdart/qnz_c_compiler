/* zero_san v1: register pressure. The checked adds must stay sound when the
 * result temp (or its operands) is spilled to the frame: the check reads the
 * same home (register or scratch register before store-back) the ALU op wrote,
 * and the post-instruction pass must not re-store scratch garbage into the
 * slot. Exit 0 = sound. */
int f(int a, int b, int c, int d, int e, int g, int h, int i, int j,
      int k, int l, int m, int n, int o, int p, int q) {
    int r1 = a + 1, r2 = b + 1, r3 = c + 1, r4 = d + 1, r5 = e + 1,
        r6 = g + 1, r7 = h + 1, r8 = i + 1, r9 = j + 1, r10 = k + 1,
        r11 = l + 1, r12 = m + 1, r13 = n + 1, r14 = o + 1, r15 = p + 1;
    int s = r1 + r2;
    int t = r3 + r4 + r5 + r6 + r7 + r8 + r9 + r10 + r11 + r12 + r13 + r14 + r15;
    return s + t + q;
}
int g64(long a, long b, long c, long d, long e, long g, long h, long i,
        long j, long k, long l, long m, long n, long o, long p, long q) {
    long r1 = a + 1, r2 = b + 1, r3 = c + 1, r4 = d + 1, r5 = e + 1,
         r6 = g + 1, r7 = h + 1, r8 = i + 1, r9 = j + 1, r10 = k + 1,
         r11 = l + 1, r12 = m + 1, r13 = n + 1, r14 = o + 1, r15 = p + 1;
    long s = r1 + r2;
    long t = r3 + r4 + r5 + r6 + r7 + r8 + r9 + r10 + r11 + r12 + r13 + r14 + r15;
    return (int)(s + t + q);
}
int main(void) {
    /* all ones: r_i = 2; s = 4, t = 13*2 = 26, +q(1) = 31 */
    if (f(1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1) != 31) return 1;
    if (f(0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0) != 15) return 2;
    /* 64-bit: all 100000L: r_i = 100001; s = 200002, t = 13*100001 = 1300013, +q = 1600015 */
    if (g64(100000L,100000L,100000L,100000L,100000L,100000L,100000L,100000L,
            100000L,100000L,100000L,100000L,100000L,100000L,100000L,100000L) != 1600015)
        return 3;
    return 0;
}
