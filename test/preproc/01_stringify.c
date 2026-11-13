/* Stringification (#) semantics - C11 6.10.3.1p2: an argument used with #
 * in the replacement list is NOT macro-expanded before substitution. */
#define X 42
#define STR(a) #a
#define STR2(a) STR(a)
#define ID(x) x
const char *a = STR(X);        /* "X" - raw, not "42" */
const char *b = STR2(X);       /* "42" - double indirection */
const char *c = STR(ID(X));    /* "ID(X)" - raw */
const char *d = STR(42);       /* "42" */
const char *e = STR(x y);      /* "x y" */
const char *f = STR();         /* "" - gcc single-#-param leniency */
const char *g = STR( (a, b) ); /* " (a, b) " */
#define MIX(a) a_ #a
const char *h = MIX(X);        /* a_ "X" - a_ is one identifier, not X_ */
#define MIX2(a) (a) #a
const char *i = MIX2(X);       /* (42) "X" - normal occurrence still expands */
#define MIX3(a) a (a) #a
const char *j = MIX3(X);       /* 42 (42) "X" */
#define MIX4(a) #a a
const char *k = MIX4(X);       /* "X" 42 */
