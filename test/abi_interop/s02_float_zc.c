/* ABI interop s02: float/double args & returns (XMM passing). */
double zc_fp(double d, float f, double e) {
    return d * 2.0 + (double)f * 3.0 - e + 0.25;
}
float zc_float_return(double d) {
    return (float)(d + 0.5);
}
