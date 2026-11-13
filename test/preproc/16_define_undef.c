/* #define / #undef syntax edges: function-like detection (no whitespace
 * between name and paren), parameter lists, redefinition, -U-style
 * redefinition, and the (empty) object macro. */
#define NO_SPACE(x) x + 1
int a = NO_SPACE(3);
#define TWO(a, b) a - b
int b = TWO(10, 4);
#define MANY(a, b, c, d) a + b + c + d
int c = MANY(1, 2, 3, 4);
#define R(x) 1
#define R(x) 2
int d = R(5);
#define OBJ 1
#undef OBJ
#define OBJ 2
int e = OBJ;
#define EMPTYOBJ
int f = 7;
#undef EMPTYOBJ
int g = 8;
#define F1(a) a
#define F2(a) a
#undef F1
int h = F2(6);
/* a #define with a parameter named like a builtin stays a parameter */
#define ID_DEFINED(x) x
#define defined_defined 1
int i = ID_DEFINED(defined_defined);
/* redefinition to the same replacement list is legal */
#define SAME 42
#define SAME 42
int j = SAME;
