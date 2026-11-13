/* Real c-torture case: compile/921227-1 - # and #undef as macro
 * arguments. A # inside an argument is an ordinary pp-token, never a
 * directive; a hash-used parameter stringifies the raw tokens, so
 * `#undef k` in an argument must NOT undefine k. */
#define k(a) #a
char *s = k(k(1,2));
char *t1 = k(#);
char *t2 = k(#undef k);
char *t3 = k(x);
#define K2(a) f(a)
K2(#undef k);
char *t4 = k(y);
char *t5 = k(#if 1);
char *t6 = k(a b c);
#undef k
char *t7 = k(z);
