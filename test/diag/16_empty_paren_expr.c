/* Empty parenthesized expression: `()` is not a valid C expression
 * (C99 6.5.9). GCC: "expected expression before ')' token".
 * Sprint 151 side finding: zerocc used to accept it and lower it to an
 * undefined-value temp, which made -O0 differential baselines
 * non-deterministic (the () read leaked register/stack garbage). */
int f(void) { return (); }
int main(void) { return f(); }
