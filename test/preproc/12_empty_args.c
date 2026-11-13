/* Empty arguments: an empty argument expands to an empty token sequence;
 * it participates normally in substitution, stringification, and pasting. */
#define E
#define P2(a, b) (a)(b)
int f1 = P2(E, 5);
#define P3(a, b) a , b
int f2 = P3(E, 7);
#define P4(a, b) a ## b
int f3 = P4(E, 8);
#define P5(a, b) a + b
int f4 = P5(9, E);
#define STR(a) #a
const char *s1 = STR(E);       /* "E" - E is hash-used: raw, not pre-expanded */
#define F2(a, b) a##b
#define G2(z) F2(z,)
int n1 = G2(w);
#define H2(a) a##E
int n2 = H2(q);                /* q - paste with empty arg */
#define CALL(a) a()
int n3 = CALL(E);              /* () */
