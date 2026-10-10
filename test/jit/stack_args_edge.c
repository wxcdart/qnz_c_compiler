/* Stack Arguments Edge Cases */
#include <assert.h>

/* Mixed integer + FP args - FP args go in xmm regs, overflow to stack */
double sum_fp7(double a, double b, double c, double d, double e, double f, double g) {
    return a + b + c + d + e + f + g;
}

/* Many integer args */
int sum20(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j,
          int k,int l,int m,int n,int o,int p,int q,int r,int s,int t) {
    return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q+r+s+t;
}

int main(void) {
    /* 7 FP args: g on stack (x86-64 has 8 FP arg regs, so this fits in regs) */
    assert((int)sum_fp7(1.0,2.0,3.0,4.0,5.0,6.0,7.0) == 28);
    
    /* 20 integer args: 14 on stack */
    int expected = 210;  // sum 1..20
    assert(sum20(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20) == expected);
    
    return 0;
}