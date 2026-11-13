/* #if / #ifdef / #elif / defined(): the expression language (C99 6.10.1)
 * includes integer constants, char constants, defined, sizeof, and the
 * usual operators; #defined identifiers in #if are replaced 1/0. */
#define A 1
#define B 2
#if A > 1 && defined(B)
int r1 = 1;
#endif
#if defined(A) + defined(NOTDEF) == 1
int r2 = 2;
#endif
#if 1 + 2 * 3 == 7
int r3 = 3;
#endif
#if 'a' == 97
int r4 = 4;
#endif
#if L'a' == 97
int r5 = 5;
#endif
#if 0xFFFFFFFFu == 4294967295u
int r6 = 6;
#endif
#if !defined(MISSING)
int r7 = 7;
#endif
#ifndef MISSING
int r8 = 8;
#else
int r8 = 9;
#endif
#if 0
/* inactive branch: arbitrary text is fine */
@#$%^&*() not c at all
#else
int r9 = 9;
#endif
#if (A)
int r10 = 10;
#elif (B)
int r10 = 11;
#else
int r10 = 12;
#endif
#if A == B
int r11 = 11;
#endif
#if 0x10 + 010 + 0b010 == 26
int r12 = 12;
#endif
#define DF 5
#if defined DF
int r13 = 13;
#endif
#undef DF
#if defined DF
int r13 = 14;
#endif
#if 1L == 1
int r15 = 15;
#endif
#if 1 ? 10 : 20 == 10
int r17 = 17;
#endif
#define F0 0
#if !F0
int r18 = 18;
#endif
