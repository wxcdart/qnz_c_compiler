/* Multi-line macros and phase-2 splicing: backslash+newline is deleted
 * before tokenization (C99 5.1.1.2) - in macro bodies, in #define
 * parameter lists, and in the code itself. */
#define ML(a) \
    (a) + \
    (a)
int a = ML(3);
int b = 1 +
        2;
#define CHAIN(a, \
             b) a * b
int c = CHAIN(3, 4);
#define SPLIT \
    40 + \
    2
int d = SPLIT;
#define TRAIL(a) a + 1
int e = TRAIL(2);
#define LASTLINE(x) x - \
    1
int f = LASTLINE(10);
int g = 5 \
        - 1;
#define MULTI(a, \
             b, \
             c) a + b + c
int h = MULTI(1, 2, 3);
