#ifndef ZERO_CC_KEYWORDS_INC
#define ZERO_CC_KEYWORDS_INC

/* C99, C11, and C23 keywords table */
typedef struct {
    const char *name;
    ZeroCCTokType type;
} KeywordEntry;

static const KeywordEntry keyword_table[] = {
    {"auto",           TK_AUTO},
    {"break",          TK_BREAK},
    {"case",           TK_CASE},
    {"char",           TK_CHAR},
    {"const",          TK_CONST},
    {"continue",       TK_CONTINUE},
    {"default",        TK_DEFAULT},
    {"do",             TK_DO},
    {"double",         TK_DOUBLE_KW},
    {"else",           TK_ELSE},
    {"enum",           TK_ENUM},
    {"extern",         TK_EXTERN},
    {"float",          TK_FLOAT_KW},
    {"for",            TK_FOR},
    {"goto",           TK_GOTO},
    {"if",             TK_IF},
    {"inline",         TK_INLINE},
    {"int",            TK_INT},
    {"long",           TK_LONG},
    {"register",       TK_REGISTER},
    {"restrict",       TK_RESTRICT},
    {"return",         TK_RETURN},
    {"short",          TK_SHORT},
    {"signed",         TK_SIGNED},
    {"sizeof",         TK_SIZEOF},
    {"static",         TK_STATIC},
    {"struct",         TK_STRUCT},
    {"switch",         TK_SWITCH},
    {"typedef",        TK_TYPEDEF},
    {"union",          TK_UNION},
    {"unsigned",       TK_UNSIGNED},
    {"void",           TK_VOID},
    {"volatile",       TK_VOLATILE},
    {"while",          TK_WHILE},

    /* C99 / C11 / C23 builtins and keywords */
    {"_Alignas",       TK_ALIGNAS},
    {"_Alignof",       TK_ALIGNOF},
    {"_Atomic",        TK_ATOMIC},
    {"_Bool",          TK_BOOL_KW},
    {"_Generic",       TK_GENERIC},
    {"_Noreturn",      TK_NORETURN},
    {"_Static_assert", TK_STATIC_ASSERT},
    {"_Thread_local",  TK_THREAD_LOCAL},
    {"__thread",       TK_THREAD_LOCAL},

    /* C23 keywords */
    {"alignas",        TK_ALIGNAS},
    {"alignof",        TK_ALIGNOF},
    {"bool",           TK_BOOL_KW},
    {"constexpr",      TK_CONSTEXPR},
    {"false",          TK_FALSE},
    {"nullptr",        TK_NULLPTR},
    {"static_assert",  TK_STATIC_ASSERT},
    {"thread_local",   TK_THREAD_LOCAL},
    {"true",           TK_TRUE},
    {"typeof",         TK_TYPEOF},
    {"typeof_unqual",  TK_TYPEOF_UNQUAL},
    {"__typeof",       TK_TYPEOF},
    {"__typeof__",     TK_TYPEOF},
    {"__typeof_unqual", TK_TYPEOF_UNQUAL},
    {"__typeof_unqual__", TK_TYPEOF_UNQUAL},
};

#define KEYWORD_COUNT (sizeof(keyword_table) / sizeof(keyword_table[0]))

#endif /* ZERO_CC_KEYWORDS_INC */
