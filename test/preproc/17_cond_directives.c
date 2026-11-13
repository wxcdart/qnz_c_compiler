/* Conditional group nesting, #elif chains, and #undef visibility:
 * #undef in an ACTIVE branch takes effect immediately; content of
 * inactive branches is skipped without being expanded. */
#define T 1
#ifdef T
int a = 1;
#endif
#ifndef T
int a = 2;
#endif
#if T
int b = 1;
#elif !T
int b = 2;
#else
int b = 3;
#endif
#if T
#ifdef T
int c = 1;
#else
int c = 2;
#endif
int d = 1;
#endif
#undef T
#ifndef T
int e = 1;
#endif
#if T
int f = 1;
#endif
#define U 3
#if 0
#ifdef U
int g = 1;
#endif
#else
#ifdef U
int g = 2;
#endif
#endif
#undef U
#ifdef U
int h = 1;
#endif
#define V 1
#if V
#define W 9
#endif
#if defined(W)
int i = W;
#endif
#define M1 4
#define M2 5
#if M1
#define KEEP 1
#elif M2
#define KEEP 2
#endif
int j = KEEP;
#undef KEEP
int k = 1;
