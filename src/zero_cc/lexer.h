/* Lexer - Modular Headers */
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

/* Forward declarations for scan functions */
static int zero_cc_scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_handle_escape(const char **s, size_t *offset, char *out_str, int *out_val);

#include "lexer/init.h"
#include "lexer/next.h"
#include "lexer/scan.h"
#include "lexer/tokenize.h"

#endif /* ZERO_CC_LEXER_H */
