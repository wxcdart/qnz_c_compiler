/* ABI interop s09: variadic functions (zerocc callee, gcc caller). */
#include <stdarg.h>
int zc_vsum(int n, ...) {
    va_list ap;
    int t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, int);
    va_end(ap);
    return t;
}
double zc_vavg(int n, ...) {
    va_list ap;
    double t = 0.0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += (double)va_arg(ap, int);
    va_end(ap);
    return n ? t / (double)n : 0.0;
}
int zc_mixed_va(const char *tag, int a, double d, ...) {
    va_list ap;
    va_start(ap, d);
    int x = va_arg(ap, int);
    va_end(ap);
    return (int)(a * 2 + d + (double)x) + (int)tag[0] % 7;
}
