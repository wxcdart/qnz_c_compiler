/* stdnoreturn.h - C11 7.2 noreturn for ZeroCC.
 *
 * Provides the standard `noreturn` keyword spelling. zerocc lexes
 * `_Noreturn` natively (TK_NORETURN), so the standard spelling maps onto
 * it. Mirrors the virtual header embedded in
 * src/zero_cc/core/preprocessor.h (kept in sync manually).
 */
#ifndef _STDNORETURN_H
#define _STDNORETURN_H
#define noreturn _Noreturn
#endif
