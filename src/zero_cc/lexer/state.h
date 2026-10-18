/* Lexer State Definition */
#ifndef ZERO_CC_LEXER_STATE_H
#define ZERO_CC_LEXER_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Lexer state */
typedef struct {
    const char *src;      /* full source string */
    const char *cur;      /* current position */
    const char *filename; /* source file name */
    size_t      line;     /* 1-based */
    size_t      col;      /* 1-based */
    size_t      offset;   /* byte offset from src start */
    char        err_msg[256]; /* specific error message if ret == -1 */
} ZeroCCLexer;

#endif /* ZERO_CC_LEXER_STATE_H */
