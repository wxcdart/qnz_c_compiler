/* String and character literals in the preprocessor: no macro expansion
 * inside literal text; adjacent literals are separate tokens (concatenation
 * is a compiler phase, but -E shows the token stream); escapes are intact. */
#define M(x) x
const char *s1 = "abc" "def";
const char *s2 = "pre" M("mid") "post";
const char *s3 = "has ## and # and MACRO names inside";
#define MACRO "not expanded in a string"
const char *s4 = MACRO;
const char *s5 = "tab\tand\nnewline and \" quote";
char c1 = '\n';
char c2 = '\'';
char c3 = '\\';
char c4 = '\?';
const char *s6 = "wide L prefix stays" L"literal";
char c5 = L'x';
