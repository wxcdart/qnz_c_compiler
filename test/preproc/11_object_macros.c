/* Object-like macros: nested expansion, redefinition after #undef, empty
 * object macros, and single-expansion semantics (a macro that expands to
 * itself in its own replacement list stays blue). */
#define OBJ 41
#define OBJ2 OBJ + 1
int a = OBJ;
int b = OBJ2;
#undef OBJ2
#define OBJ2 7
int c = OBJ2;
#define EMPTY
int d = 10 + EMPTY;
#define NEST1 NEST2
#define NEST2 99
int e = NEST1;
#define SELF2 OBJ2 OBJ2
int f = SELF2;
#define NUM1 5
#define NUM2 6
#define SUM(a, b) ((a) + (b))
int g = SUM(NUM1, NUM2);
#undef NUM1
#define NUM1 100
int h = SUM(NUM1, NUM2);
#define PAREN(x) (x)
int i = PAREN(PAREN(3));
#define STAR *
int j = 2 STAR 3;
