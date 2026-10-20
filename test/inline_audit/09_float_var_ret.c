/* Inlined return of a FLOAT VARIABLE. */
static float fvar(float x) { return x; }
int main(void) {
    float r = fvar(2.5f);
    if (r != 2.5f) return 1;
    return 0;
}
