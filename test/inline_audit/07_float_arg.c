/* float/double args and return. */
static double scale(double x, float f) { return x * 2.0 + (double)f; }
int main(void) {
    double d = 1.5;
    float f = 0.25f;
    double r = scale(d, f);
    if (r != 3.25) return 1;
    return 0;
}
