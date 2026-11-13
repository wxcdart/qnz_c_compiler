/* Preprocessing-number atomicity: a pp-number is the maximal sequence of
 * digits, e-signs, p-exponents, and float suffixes (C99 6.4.4). `0.0F`
 * must stay one token even when `F` is a macro. */
int f1 = 0.0F;
#define F(x) x
int f2 = 0.0 F;
double d1 = 1e5;
double d2 = 1.E5;
double d3 = .5;
double d4 = 1.;
double d5 = 1E-3;
double d6 = 0x1.8p3;
double d7 = 0X1.8P3;
double d8 = 0x1p-2;
float fl1 = 1.5f;
float fl2 = 2.5F;
long l1 = 100UL;
long l2 = 100 U L;
unsigned u1 = 42u;
unsigned u2 = 0xFFU;
long long ll1 = 1234567890123456789LL;
unsigned long long ull1 = 18446744073709551615ULL;
char c1 = '\xff';
char c2 = '\x7F';
char c3 = '\u0041';
char c4 = '\n';
char c5 = '\'';
char c6 = '\\';
int sep1 = 1'000'000;
long sep2 = 123'456'789L;
float sep3 = 1.5'0f;
int hex1 = 0xabc;
int oct1 = 017;
int bin1 = 0b1010;
