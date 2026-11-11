/* ABI interop s02: gcc-side FP provider. */
#include <math.h>
double gc_fp(double d, float f, double e) {
    return sqrt(d) * (double)f + e * 0.5 - 1.0;
}
float gc_float_return(double d) {
    return (float)(d - 0.5);
}
