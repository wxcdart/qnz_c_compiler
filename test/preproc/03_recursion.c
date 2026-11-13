/* Recursive and self-referencing macros: a macro is blue while it expands,
 * so a self-reference in the substitution result (or in a pre-expanded
 * argument) stays unexpanded. */
#define STR(a) #a
#define TEN(x) x x x x x x x x x x
#define SELF(x) SELF(x)
const char *a = STR(SELF(1));   /* "SELF(1)" - inner SELF is blue */
const char *b = STR(TEN(1));    /* "1 1 1 1 1 1 1 1 1 1" */
int TEN2 = 0;
/* c-torture compile/20050423-1 shape: nested self-reference in an
 * argument that is itself a statement list. */
void foo(int x, int y)
{
    register int a = y + 57;
    register int b = y + 31;
    while (x-- > 0)
    {
#define TEN(x) x x x x x x x x x x
        TEN (TEN (a += b; b -= a;))
        TEN (TEN (a += b; b -= a;))
    }
}
#define A(x) B(x)
#define B(x) A(x)
const char *c = STR(A(1));      /* "A(1)" - mutual cycle cut by blue paint */
#define C1(x) x
#define C2(x) C1(x)
#define C3(x) C2(x)
int d = C3(1 + 2);
int e = C1(C2(C3(4)));
