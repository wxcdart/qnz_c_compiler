/* Soft case: C99 requires a diagnostic; gcc 13 warns (implicit-decl),
 * zerocc currently accepts the implicit declaration silently. */
int main(void) { undeclared_thing(1, 2); return 0; }
