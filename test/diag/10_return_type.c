/* Non-scalar (struct) return from an int function -> incompatible-types.
 * (A pointer/string-literal return into int is legal C89/GNU — gcc
 * -Wint-conversion warns but accepts, so it is not a battery case.) */
struct S { int a, b; };
int f(void) { struct S s; s.a = 1; return s; }
int main(void) { return f(); }
