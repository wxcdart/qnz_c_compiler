#ifndef ZERO_CC_LEXER_H
#define ZERO_CC_LEXER_H

#include "types_lexer.h"



/* Lexer state */
typedef struct {
    const char *src;      /* full source string */
    const char *cur;      /* current position */
    const char *filename; /* source file name */
    size_t      line;     /* 1-based */
    size_t      col;      /* 1-based */
    size_t      offset;   /* byte offset from src start */
} ZeroCCLexer;

/* Initialize lexer */
void zero_cc_lexer_init(ZeroCCLexer *lx, const char *src);
void zero_cc_lexer_init_file(ZeroCCLexer *lx, const char *src, const char *filename);

/* Get next token; returns 0 on success, 1 on EOF, -1 on error */
int zero_cc_lexer_next(ZeroCCLexer *lx, ZeroCCToken *out);

/* Convenience: tokenize entire source into compiler's token array */
void zero_cc_tokenize_c99(const char *src, ZeroCCCompiler *c);

/* Keyword lookup using perfect hash from generated table */
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len);

#endif /* ZERO_CC_LEXER_H */