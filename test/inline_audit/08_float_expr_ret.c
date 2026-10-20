/* Inlined return of a FLOAT EXPRESSION (exercises FP return redirection). */
static double fexpr(double a, double b) { return a + b * 3.0 - 1.0; }
int main(void) {
    double r = fexpr(1.0, 2.0);
    if (r != 6.0) return 1;
    return 0;
}
