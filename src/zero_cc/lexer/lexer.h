/* Lexer - Modular Headers */
#ifndef ZERO_CC_LEXER_H
#define ZERO_CC_LEXER_H

#include "state.h"

/* Forward declaration for token - needed before types/types.h */
typedef struct ZeroCCToken ZeroCCToken;

/* Now include type system */
#include "../types/types.h"

/* Forward declarations for scan functions */
static int zero_cc_scan_number(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_string(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_char(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_identifier(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_scan_operator(ZeroCCLexer *lx, ZeroCCToken *tok);
static int zero_cc_handle_escape(const char **s, size_t *offset, char *out_str, int *out_val);

#include "init.h"
#include "next.h"
#include "scan.h"
#include "tokenize.h"

#endif /* ZERO_CC_LEXER_H */
