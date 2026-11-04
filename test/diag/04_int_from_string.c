/* Struct -> int assignment -> incompatible-types.
 * (A pointer/string-literal assignment into int is legal C89/GNU — gcc
 * -Wint-conversion warns but accepts, so it is not a battery case.) */
struct S { int a; };
int main(void) { int x; struct S s; s.a = 1; x = s; return x; }
