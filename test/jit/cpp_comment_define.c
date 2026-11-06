/* Sprint 118: #define macro definitions with trailing comments.
 *
 * C99 phase 1/2 comment handling:
 * 1. A // line comment ends the macro value up to newline.
 * 2. A block comment starting mid-#define line and continuing across
 *    following lines must be replaced with a single space, without leaking
 *    its continuation lines into the token stream as stray code.
 *
 * Tested in both object-like and function-like macros. */

#define VAL_LINE_COMM 10 // trailing line comment
#define VAL_MULTI_BLOCK 20 /* block comment
                            * continuation line 1
                            * continuation line 2 */
#define VAL_MID_BLOCK 30 /* first */ + 5 /* second
                                          * multi-line */
#define FN_LINE_COMM(x) ((x) * 2) // double the input
#define FN_MULTI_BLOCK(x) ((x) + 100) /* block comment on fn macro
                                       * second line */

int main(void) {
    if (VAL_LINE_COMM != 10) return 1;
    if (VAL_MULTI_BLOCK != 20) return 2;
    if (VAL_MID_BLOCK != 35) return 3;
    if (FN_LINE_COMM(5) != 10) return 4;
    if (FN_MULTI_BLOCK(25) != 125) return 5;
    return 0;
}
