#ifndef ZERO_CC_TYPES_TOKEN_H
#define ZERO_CC_TYPES_TOKEN_H

/* --------------------------------------------------------------------------
 *  3. STEP 2 & 3: ADVANCED C99 TYPE SYSTEM & COMPILER ENGINE
 *  -------------------------------------------------------------------------- */

#include "zero_cc/core.h"
/* Forward declarations for backend structs – needed because zero_cc_backends.h
   refers to them before they are defined. */
typedef struct ZeroCCEmitBackend   ZeroCCEmitBackend;
typedef struct ZeroCCLinkBackend   ZeroCCLinkBackend;
typedef struct ZeroCCMemoryBackend ZeroCCMemoryBackend;
typedef struct ZeroCCABICallBackend ZeroCCABICallBackend;
typedef struct ZeroCCAtomicBackend ZeroCCAtomicBackend;
typedef struct ZeroCCGenericBackend ZeroCCGenericBackend;
typedef struct ZeroCCStdlibBackend ZeroCCStdlibBackend;
typedef struct ZeroCCDiagBackend   ZeroCCDiagBackend;
#include "zero_cc/zero_cc_backends.h"

typedef enum {
    TK_EOF = 0,
    TK_NUM, TK_STRING, TK_IDENT,
    /* Keywords */
    TK_INT, TK_CHAR, TK_VOID, TK_STRUCT, TK_UNION, TK_TYPEDEF, TK_RETURN,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO, TK_BREAK, TK_CONTINUE,
    TK_SIZEOF, TK_STATIC, TK_INLINE,
    TK_ENUM, TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_FLOAT_KW, TK_DOUBLE_KW, TK_BOOL_KW, TK_SIGNED, TK_UNSIGNED, TK_SHORT, TK_LONG,
    TK_AUTO, TK_GENERIC, TK_NULLPTR, TK_TRUE, TK_FALSE,
    TK_STATIC_ASSERT, TK_ALIGNAS, TK_ALIGNOF,
    TK_NORETURN, TK_THREAD_LOCAL, TK_ATOMIC,
    TK_TYPEOF, TK_TYPEOF_UNQUAL,
    /* Missing C keywords */
    TK_CONST, TK_EXTERN, TK_REGISTER, TK_RESTRICT, TK_VOLATILE, TK_GOTO,
    TK_CONSTEXPR,
    /* Operators & Punctuation */
    TK_SEMI, TK_COLON, TK_QUESTION, TK_LBRACE, TK_RBRACE, TK_LPAREN, TK_RPAREN, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_ASSIGN, TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_BANG,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_LOGAND, TK_LOGOR, TK_SHL, TK_SHR,
    TK_ARROW, TK_DOT,
    TK_HASH, TK_PASTE,
    TK_ELLIPSIS,
    TK_FLOAT, TK_CHAR_LIT,
    TK_INC, TK_DEC,
    /* Compound assignment operators */
    TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MUL_ASSIGN, TK_DIV_ASSIGN,
    TK_MOD_ASSIGN, TK_AND_ASSIGN, TK_OR_ASSIGN, TK_XOR_ASSIGN, TK_SHL_ASSIGN, TK_SHR_ASSIGN,
    /* C23 double colon and attribute double brackets */
    TK_COLONCOLON, TK_LDBRACKET, TK_RDBRACKET
} ZeroCCTokType;

typedef struct ZeroCCToken {
    ZeroCCTokType type;
    int64_t num_val;
    double float_val;
    uint8_t int_suffix; /* bitflags: 1=U,2=L,4=LL */
    char str_val[128];
    char *allocated_str; /* for long strings (> 127 chars) */
    const char *filename; /* source filename if available */
    size_t line;      /* 1-based line number */
    size_t col;       /* 1-based column number */
    const char *src;   /* pointer into original source */
    size_t len;        /* length of token in source */
} ZeroCCToken;

typedef ZeroCCToken ZeroCCTokenV2;

/* Type System */

#endif /* ZERO_CC_TYPES_TOKEN_H */
