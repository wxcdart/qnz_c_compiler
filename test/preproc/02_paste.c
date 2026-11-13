/* Token pasting (##) semantics - C11 6.10.3.3: whitespace around ## is
 * discarded; an argument used with ## is NOT macro-expanded first. */
#define CAT(a, b) a##b
#define Y y
#define X 42
int CAT(x, 1) = 1;
int n1 = CAT(v, 2);
const char *s1 = Y##1;          /* Y1 */
#define YCAT(a) Y##a
const char *s2 = YCAT(1);       /* Y1 */
const char *s3 = YCAT(X);       /* YX - raw, not Y42 */
#define P1(x) x##1
int n2 = P1(v);                 /* v1 */
int CAT(x,) = 2;                /* GNU: paste with empty arg -> x */
#define F2(a, b) a##b
#define G2(z) F2(z,)
int n3 = G2(w);                 /* w - empty arg dropped by paste */
#define WS(a, b) a ## b
int n4 = WS(v, 3);              /* v3 - whitespace discarded */
#define WSP(a, b) a## b
int n5 = WSP(v, 4);             /* v4 */
#define WSP2(a, b) a ##b
int n6 = WSP2(v, 5);            /* v5 */
#define DIG1(a) a##0x1
int n7 = DIG1(0x2);             /* 0x20x1 - paste forms the token */
#define DOT(a, b) a.b
int n8 = DOT(1, 5);             /* 1.5 */
#define DOT2(a, b) a##b
int n9 = DOT2(1, .5);           /* 1.5 - number paste across the dot */
