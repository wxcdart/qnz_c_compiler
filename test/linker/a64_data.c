/* AArch64 own-linker coverage: globals + static string (adrp/add pairs,
 * .data-only object), cross-function calls. */
int g1 = 7;
int g2;
static const char msg[] = "hello %d\n";
const char *s(void) { return msg; }
int use(int v) { g2 = g1 + v; return (int)s()[0] - 96; }
int main(void) { return (use(2) + g2 + g1 == 24) ? 0 : 1; }
