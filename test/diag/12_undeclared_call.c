/* Soft case: gnu89 implicit function declaration. GCC accepts the call
 * with an -Wimplicit-function-declaration warning (not an error); zerocc
 * matches: the callee gets an implicit `int name(...)` declaration at the
 * call site with a warning (Sprint 99), so this compiles (rc=0) with a
 * warning and no error diagnostic. */
int main(void) { undeclared_thing(1, 2); return 0; }
