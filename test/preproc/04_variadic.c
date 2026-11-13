/* Variadic macros (__VA_ARGS__): each argument expands independently;
 * the variadic slot is the comma-joined remainder. An argument used with
 * # or ## stays raw even in the variadic slot. */
#define V(...) __VA_ARGS__
int a = V(1, 2, 3);
int b = V(4);
#define NV(x, ...) x __VA_ARGS__
int c = NV(a, b, c);            /* a b c */
int d = NV(7);                  /* 7 - empty rest */
#define SV(...) #__VA_ARGS__
const char *e = SV(1, 2, 3);    /* "1, 2, 3" */
const char *f = SV(9);          /* "9" */
#define X 42
const char *g = SV(X);          /* "X" - raw (used with #) */
#define SV2(a, ...) a, #__VA_ARGS__
const char *h = SV2(1, 2, 3);   /* 1, "2, 3" */
#define VP(a, ...) a##__VA_ARGS__
int i = VP(v, 7);               /* v7 */
#define VW(x, ...) x, ##__VA_ARGS__
int j = VW(a);                  /* a - GNU empty-variadic comma drop */
int k = VW(a, b, c);            /* a, b, c */
#define VW2(x, ...) __VA_ARGS__, x
int l = VW2(q, 1, 2);           /* 1, 2, q */
