/* Macro expansion semantics: arguments expand fully (unless used with
 * #/##), the substitution result rescans, a macro's own name stays blue
 * through its expansion, and nested invocations expand innermost-out. */
#define ONCE(x) x
#define WRAP(y) ONCE(y)
int a = WRAP(5);
#define NESTED(x) f(x)
#define INNER(x) x * 2
int b = NESTED(INNER(3));
#define ARGS(x, y) x##y
int c = ARGS(a, b);
int d = ARGS(1, 2);
#define TERN(x, y, z) x ? y : z
int e = TERN(1, 2, 3);
#define COMMA(a, b) (a, b)
int f = COMMA(1, 2);
#define PAREN(a) (a)
int g = PAREN(1) + PAREN(2);
#define DOUBLE(x, y) x y
int h = DOUBLE(1, 2);
#define SELF3(x) SELF3
const char *i = SELF3(1);
#define K1(x) x
#define K2(x) K1(x) K1(x)
int j = K2(5);
#define SEQ(a, b, c) a b c
#define X1 1
#define X2 2
#define X3 3
int k = SEQ(X1, X2, X3);
