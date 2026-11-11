/* ABI interop: s02 float/double args & returns (shared main, compiled by BOTH compilers)
 * expect: 19.75
 * expect: 10.50
 */
#include <stdio.h>
double zc_fp(double, float, double); float zc_float_return(double);
double gc_fp(double, float, double); float gc_float_return(double);
int main(void) {
    double a = zc_fp(4.0, 2.0f, 1.5) + gc_fp(9.0, 2.0f, 4.0);
    printf("%.2f\n", a);
    printf("%.2f\n", (double)zc_float_return(3.25) + (double)gc_float_return(7.25));
    return 0;
}

