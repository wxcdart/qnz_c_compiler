/* #pragma handling: gcc -E strips every pragma from the output (known and
 * unknown alike); side effects (message note, diagnostic state) do not
 * affect stdout. zerocc must strip identically (-E) while still honoring
 * pack at compile time. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
int a = 1;
#pragma GCC diagnostic pop
#pragma message "hello from pragma"
int b = 2;
#pragma unknown_thing foo
int c = 3;
#pragma pack(push, 1)
struct S { char a; int b; char c; };
#pragma pack(pop)
int d = 4;
#pragma GCC diagnostic warning "-Wuninitialized"
int e = 5;
