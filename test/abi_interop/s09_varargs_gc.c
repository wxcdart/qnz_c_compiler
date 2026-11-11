/* ABI interop s09: gcc-side variadic provider (zerocc caller). */
#include <stdarg.h>
long long gc_varange(int first, ...) {
    va_list ap;
    long long t = first;
    va_start(ap, first);
    while (1) {
        int v = va_arg(ap, int);
        if (v == -1) break;
        t += v;
    }
    va_end(ap);
    return t;
}
double gc_prod(int n, ...) {
    va_list ap;
    double t = 1.0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t *= (double)va_arg(ap, int);
    va_end(ap);
    return t;
}
