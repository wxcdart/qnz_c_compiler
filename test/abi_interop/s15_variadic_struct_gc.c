/* ABI interop s15: gcc-side variadic struct functions. */
#include <stdarg.h>
typedef struct { long long v[4]; } Big;
typedef struct { double a; long long b; } M16;
long long gc_bigsum(int n, ...) {
    va_list ap;
    long long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, long long);
    Big x = va_arg(ap, Big);
    va_end(ap);
    return t + x.v[0] + x.v[1] + x.v[2] + x.v[3];
}
double gc_msum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) { (void)va_arg(ap, int); (void)va_arg(ap, double); }
    M16 m = va_arg(ap, M16);
    va_end(ap);
    return m.a + (double)m.b;
}
long long gc_nbig(Big x, int n, ...) {
    va_list ap;
    long long t = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) t += va_arg(ap, long long);
    va_end(ap);
    return x.v[0] + t;
}
