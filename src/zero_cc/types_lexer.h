/* --------------------------------------------------------------------------
 *  3. STEP 2 & 3: ADVANCED C99 TYPE SYSTEM & COMPILER ENGINE
 *  -------------------------------------------------------------------------- */

#include "core.h"
#ifndef ZERO_CC_TYPES_LEXER_H
#define ZERO_CC_TYPES_LEXER_H

typedef enum {
    TK_EOF = 0,
    TK_NUM, TK_STRING, TK_IDENT,
    /* Keywords */
    TK_INT, TK_CHAR, TK_VOID, TK_STRUCT, TK_TYPEDEF, TK_RETURN,
    TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_DO, TK_BREAK, TK_CONTINUE,
    TK_SIZEOF, TK_STATIC, TK_INLINE,
    TK_ENUM, TK_SWITCH, TK_CASE, TK_DEFAULT,
    TK_FLOAT_KW, TK_DOUBLE_KW, TK_BOOL_KW, TK_SIGNED, TK_UNSIGNED, TK_SHORT, TK_LONG,
    TK_AUTO, TK_GENERIC, TK_NULLPTR, TK_TRUE, TK_FALSE,
    TK_STATIC_ASSERT, TK_ALIGNAS, TK_ALIGNOF,
    TK_NORETURN, TK_THREAD_LOCAL, TK_ATOMIC,
    TK_TYPEOF, TK_TYPEOF_UNQUAL,
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
    TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MUL_ASSIGN, TK_DIV_ASSIGN
} ZeroCCTokType;

typedef struct {
    ZeroCCTokType type;
    int64_t num_val;
    double float_val;
    uint8_t int_suffix; /* bitflags: 1=U,2=L,4=LL */
    char str_val[128];
} ZeroCCTokenV2;

/* Type System */
typedef enum {
    TYPE_VOID,
    TYPE_BOOL,
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_INT32,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_NULLPTR,
    TYPE_PTR,
    TYPE_ARRAY,
    TYPE_STRUCT,
    /* Reserved range for JIT intrinsics & SIMD vector types */
    TYPE_USER_0,
    TYPE_USER_1,
    TYPE_USER_2,
    TYPE_USER_3
} ZeroTypeTag;

struct ZeroCCType;
typedef struct ZeroCCStructMember {
    char name[128];
    ZeroTypeTag tag;
    size_t size;
    size_t offset;
    size_t align;
    struct ZeroCCType *base;
} ZeroCCStructMember;

typedef struct ZeroCCType {
    ZeroTypeTag tag;
    char struct_name[128];
    size_t size;
    size_t align;
    struct ZeroCCType *base;              /* For PTR and ARRAY */
    size_t array_len;                     /* For ARRAY */
    ZeroCCStructMember members[64];       /* For STRUCT */
    bool is_atomic;                       /* For _Atomic */

    size_t member_count;
} ZeroCCType;

/* Type Definitions & Struct Registry */
typedef struct {
    char alias[128];
    ZeroCCType type;
} ZeroCCTypedef;

typedef struct {
    char name[128];
    ZeroCCType type;
} ZeroCCStructDef;

typedef struct {
    char name[128];
    int64_t value;
} ZeroCCEnumConst;

/* Local Symbol Representation */
typedef struct {
    char name[128];
    ZeroCCType type;
    int32_t stack_offset;                 /* Offset from RBP */
    bool is_param;
    int param_reg_idx;
} ZeroCCSym;

/* Function Definition Record */
typedef struct {
    char name[128];
    size_t text_offset;                   /* Byte offset in text_section */
    ZeroCCType return_type;
    size_t param_count;
    char param_names[8][128];
    ZeroCCType param_types[8];
    bool returns_struct;
    size_t ret_struct_sz;
    bool is_variadic;
    bool noreturn;
} ZeroCCFunction;

/* Relocation Record */
typedef struct {
    size_t text_patch_offset;
    char target_symbol[128];
    bool is_call;
} ZeroCCReloc;

typedef struct {
    size_t text_patch_offset;
    size_t rodata_offset;
} ZeroCCStringReloc;

/* Compiler & Emitter Context */
typedef struct {
    ZeroCCTokenV2 *tokens;
    size_t token_count;
    size_t token_pos;
    int last_expr_is_float;

    ZeroCCSym locals[ZERO_CC_MAX_LOCALS];
    size_t local_count;
    int32_t current_stack_frame;

    ZeroCCTypedef typedefs[ZERO_CC_MAX_TYPEDEFS];
    size_t typedef_count;

    ZeroCCStructDef structs[ZERO_CC_MAX_STRUCTS];
    size_t struct_count;

    ZeroCCEnumConst enum_constants[ZERO_CC_MAX_ENUMS];
    size_t enum_const_count;

    ZeroCCFunction functions[ZERO_CC_MAX_FUNCTIONS];
    size_t function_count;

    ZeroCCReloc relocs[ZERO_CC_MAX_RELOCS];
    size_t reloc_count;

    ZeroCCStringReloc str_relocs[ZERO_CC_MAX_STR_RELOCS];
    size_t str_reloc_count;

    ZeroCCBuffer text_section;            /* Machine bytecode (.text) */
    ZeroCCBuffer rodata_section;          /* Read-only string constants (.rodata) */
    ZeroCCBuffer data_section;            /* Initialized globals (.data) */
    int had_error;
    char error_msg[256];
} ZeroCCCompiler;

int64_t decl_align_spec; /* alignment specifier from _Alignas */

/* Base Types */
static inline ZeroCCType zero_cc_type_void(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_VOID; t.size = 0; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_bool(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_BOOL; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_char(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT8; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_uint8(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT8; t.size = 1; t.align = 1;
    return t;
}
static inline ZeroCCType zero_cc_type_int16(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT16; t.size = 2; t.align = 2;
    return t;
}
static inline ZeroCCType zero_cc_type_uint16(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT16; t.size = 2; t.align = 2;
    return t;
}
static inline ZeroCCType zero_cc_type_int(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT32; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_uint32(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT32; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_int64(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_INT64; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_uint64(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_UINT64; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_float(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_FLOAT; t.size = 4; t.align = 4;
    return t;
}
static inline ZeroCCType zero_cc_type_double(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_DOUBLE; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_nullptr(void) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_NULLPTR; t.size = 8; t.align = 8;
    return t;
}
static inline ZeroCCType zero_cc_type_user(int user_idx, size_t size, size_t align) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = (ZeroTypeTag)(TYPE_USER_0 + (user_idx & 3));
    t.size = size ? size : 16;
    t.align = align ? align : 16;
    return t;
}
static inline ZeroCCType zero_cc_type_ptr(ZeroCCType *base) {
    ZeroCCType t; memset(&t, 0, sizeof(t));
    t.tag = TYPE_PTR; t.size = 8; t.align = 8;
    t.base = base;
    return t;
}

static inline ZeroCCType zero_cc_type_array(ZeroCCType *base, ssize_t length) {
    ZeroCCType t; memset(&t, 0, sizeof(t)); t.tag = TYPE_ARRAY; t.base = base; t.array_len = length; if (length > 0) { t.size = base->size * length; } else { t.size = 0; } t.align = base->align; return t;
}

/* Struct Registry */
static inline void zero_cc_register_struct(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    for (size_t i = 0; i < c->struct_count; i++) {
        if (strcmp(c->structs[i].name, name) == 0) {
            c->structs[i].type = type;
            return;
        }
    }
    if (c->struct_count < ZERO_CC_MAX_STRUCTS) {
        snprintf(c->structs[c->struct_count].name, 128, "%s", name);
        c->structs[c->struct_count].type = type;
        c->struct_count++;
    }
}

static inline ZeroCCType* zero_cc_find_struct(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->struct_count; i++) {
        if (strcmp(c->structs[i].name, name) == 0) {
            return &c->structs[i].type;
        }
    }
    return NULL;
}

/* Typedef Registry */
static inline void zero_cc_register_typedef(ZeroCCCompiler *c, const char *alias, ZeroCCType type) {
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (strcmp(c->typedefs[i].alias, alias) == 0) {
            c->typedefs[i].type = type;
            return;
        }
    }
    if (c->typedef_count < ZERO_CC_MAX_TYPEDEFS) {
        snprintf(c->typedefs[c->typedef_count].alias, 128, "%s", alias);
        c->typedefs[c->typedef_count].type = type;
        c->typedef_count++;
    }
}

static inline ZeroCCType* zero_cc_find_typedef(ZeroCCCompiler *c, const char *alias) {
    for (size_t i = 0; i < c->typedef_count; i++) {
        if (strcmp(c->typedefs[i].alias, alias) == 0) {
            return &c->typedefs[i].type;
        }
    }
    return NULL;
}

/* Enum Constant Registry */
static inline void zero_cc_register_enum_const(ZeroCCCompiler *c, const char *name, int64_t val) {
    for (size_t i = 0; i < c->enum_const_count; i++) {
        if (strcmp(c->enum_constants[i].name, name) == 0) {
            c->enum_constants[i].value = val;
            return;
        }
    }
    if (c->enum_const_count < ZERO_CC_MAX_ENUMS) {
        snprintf(c->enum_constants[c->enum_const_count].name, 128, "%s", name);
        c->enum_constants[c->enum_const_count].value = val;
        c->enum_const_count++;
    }
}

static inline ZeroCCEnumConst* zero_cc_find_enum_const(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->enum_const_count; i++) {
        if (strcmp(c->enum_constants[i].name, name) == 0) {
            return &c->enum_constants[i];
        }
    }
    return NULL;
}

static inline void zero_cc_init_standard_types(ZeroCCCompiler *c) {
    zero_cc_register_typedef(c, "void", zero_cc_type_void());
    zero_cc_register_typedef(c, "bool", zero_cc_type_bool());
    zero_cc_register_typedef(c, "_Bool", zero_cc_type_bool());
    zero_cc_register_typedef(c, "char", zero_cc_type_char());
    zero_cc_register_typedef(c, "int8_t", zero_cc_type_char());
    zero_cc_register_typedef(c, "uint8_t", zero_cc_type_uint8());
    zero_cc_register_typedef(c, "unsigned char", zero_cc_type_uint8());
    zero_cc_register_typedef(c, "short", zero_cc_type_int16());
    zero_cc_register_typedef(c, "int16_t", zero_cc_type_int16());
    zero_cc_register_typedef(c, "uint16_t", zero_cc_type_uint16());
    zero_cc_register_typedef(c, "unsigned short", zero_cc_type_uint16());
    zero_cc_register_typedef(c, "int", zero_cc_type_int());
    zero_cc_register_typedef(c, "int32_t", zero_cc_type_int());
    zero_cc_register_typedef(c, "uint32_t", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "unsigned", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "unsigned int", zero_cc_type_uint32());
    zero_cc_register_typedef(c, "long", zero_cc_type_int64());
    zero_cc_register_typedef(c, "int64_t", zero_cc_type_int64());
    zero_cc_register_typedef(c, "long long", zero_cc_type_int64());
    zero_cc_register_typedef(c, "uint64_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "unsigned long", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "unsigned long long", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "size_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "uintptr_t", zero_cc_type_uint64());
    zero_cc_register_typedef(c, "intptr_t", zero_cc_type_int64());
    zero_cc_register_typedef(c, "float", zero_cc_type_float());
    zero_cc_register_typedef(c, "double", zero_cc_type_double());
}

/* Lexer implementation */
static inline void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c) {
    size_t cap = 1024;
    c->tokens = (ZeroCCTokenV2 *)malloc(cap * sizeof(ZeroCCTokenV2));
    c->token_count = 0;
    c->token_pos = 0;
    const char *s = p;
    while (s && *s) {
        /* Skip whitespace and simple comments (line comments) */
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;

        if (c->token_count + 2 >= cap) {
            cap *= 2;
            c->tokens = (ZeroCCTokenV2 *)realloc(c->tokens, cap * sizeof(ZeroCCTokenV2));
        }

        ZeroCCTokenV2 tok;
        memset(&tok, 0, sizeof(tok));

        /* Line comment */
        if (s[0] == '/' && s[1] == '/') {
            s += 2;
            while (*s && *s != '\n') s++;
            continue;
        }
        /* Block comment */
        if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) s++;
            if (*s) s += 2;
            continue;
        }

        /* Character literal: 'a' or '\n' */
        if (*s == '\'') {
            s++;
            int val = 0;
            if (*s == '\\' && s[1]) {
                s++;
                if (*s == 'n') val = '\n';
                else if (*s == 't') val = '\t';
                else if (*s == 'r') val = '\r';
                else if (*s == '\\') val = '\\';
                else if (*s == '\'') val = '\'';
                else if (*s == '0') val = '\0';
                else val = (unsigned char)*s;
                s++;
            } else if (*s) {
                val = (unsigned char)*s;
                s++;
            }
            if (*s == '\'') s++;
            tok.type = TK_CHAR_LIT;
            tok.num_val = val;
            c->tokens[c->token_count++] = tok;
            continue;
        }

        /* Numeric or floating-point constants */
        if (isdigit((unsigned char)*s) || (*s == '.' && isdigit((unsigned char)s[1]))) {
            /* Try parsing as floating-point if contains '.' or exponent */
            char *endptr_d = NULL;
            double dv = strtod(s, &endptr_d);
            if (endptr_d && endptr_d != s) {
                /* determine if parsed characters included a decimal point or exponent */
                bool has_fp = false;
                for (char *q = (char*)s; q < endptr_d; q++) {
                    if (*q == '.' || *q == 'e' || *q == 'E') { has_fp = true; break; }
                }
                if (has_fp) {
                    tok.type = TK_FLOAT;
                tok.float_val = dv;
                /* skip float suffixes like f, F, l, L */
                s = endptr_d;
                if (*s == 'f' || *s == 'F' || *s == 'l' || *s == 'L') s++;
                c->tokens[c->token_count++] = tok;
                    continue;
                }
            }

            /* Otherwise integer */
            tok.type = TK_NUM;
            char *endptr;
            tok.num_val = strtoll(s, &endptr, 0);
            /* parse integer suffixes like U, L, UL, LL (order-insensitive) */
            uint8_t suf = 0;
            char *p = endptr;
            while (*p) {
                if (*p == 'u' || *p == 'U') { suf |= 1; p++; }
                else if ((*p == 'l' || *p == 'L') && (p[1] == 'l' || p[1] == 'L')) { suf |= 4; p += 2; }
                else if (*p == 'l' || *p == 'L') { suf |= 2; p++; }
                else break;
            }
            tok.int_suffix = suf;
            s = p;
            c->tokens[c->token_count++] = tok;
            continue;
        }

        /* String literal with concatenation support */
        if (*s == '"') {
            tok.type = TK_STRING;
            size_t sl = 0;
            while (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '\\' && s[1]) {
                        s++;
                        if (*s == 'n') tok.str_val[sl++] = '\n';
                        else if (*s == 't') tok.str_val[sl++] = '\t';
                        else if (*s == '0') tok.str_val[sl++] = '\0';
                        else tok.str_val[sl++] = *s;
                        s++;
                    } else {
                        if (sl < sizeof(tok.str_val)-1) tok.str_val[sl++] = *s;
                        s++;
                    }
                }
                if (*s == '"') s++;
                while (*s && isspace((unsigned char)*s)) s++;
            }
            tok.str_val[sl] = '\0';
            c->tokens[c->token_count++] = tok;
            continue;
        }

        /* Identifier or keyword */
        if (isalpha((unsigned char)*s) || *s == '_') {
            size_t il = 0;
            while ((isalnum((unsigned char)*s) || *s == '_') && il < sizeof(tok.str_val)-1) tok.str_val[il++] = *s++;
            tok.str_val[il] = '\0';

            /* keywords mapping */
            if (strcmp(tok.str_val, "return") == 0) tok.type = TK_RETURN;
            else if (strcmp(tok.str_val, "if") == 0) tok.type = TK_IF;
            else if (strcmp(tok.str_val, "else") == 0) tok.type = TK_ELSE;
            else if (strcmp(tok.str_val, "while") == 0) tok.type = TK_WHILE;
            else if (strcmp(tok.str_val, "for") == 0) tok.type = TK_FOR;
            else if (strcmp(tok.str_val, "do") == 0) tok.type = TK_DO;
            else if (strcmp(tok.str_val, "break") == 0) tok.type = TK_BREAK;
            else if (strcmp(tok.str_val, "continue") == 0) tok.type = TK_CONTINUE;
            else if (strcmp(tok.str_val, "struct") == 0) tok.type = TK_STRUCT;
            else if (strcmp(tok.str_val, "typedef") == 0) tok.type = TK_TYPEDEF;
            else if (strcmp(tok.str_val, "enum") == 0) tok.type = TK_ENUM;
            else if (strcmp(tok.str_val, "switch") == 0) tok.type = TK_SWITCH;
            else if (strcmp(tok.str_val, "case") == 0) tok.type = TK_CASE;
            else if (strcmp(tok.str_val, "default") == 0) tok.type = TK_DEFAULT;
            else if (strcmp(tok.str_val, "sizeof") == 0) tok.type = TK_SIZEOF;
            else if (strcmp(tok.str_val, "static") == 0) tok.type = TK_STATIC;
            else if (strcmp(tok.str_val, "inline") == 0) tok.type = TK_INLINE;
            else if (strcmp(tok.str_val, "int") == 0) tok.type = TK_INT;
            else if (strcmp(tok.str_val, "char") == 0) tok.type = TK_CHAR;
            else if (strcmp(tok.str_val, "void") == 0) tok.type = TK_VOID;
            else if (strcmp(tok.str_val, "float") == 0) tok.type = TK_FLOAT_KW;
            else if (strcmp(tok.str_val, "double") == 0) tok.type = TK_DOUBLE_KW;
            else if (strcmp(tok.str_val, "bool") == 0 || strcmp(tok.str_val, "_Bool") == 0) tok.type = TK_BOOL_KW;
            else if (strcmp(tok.str_val, "signed") == 0) tok.type = TK_SIGNED;
            else if (strcmp(tok.str_val, "unsigned") == 0) tok.type = TK_UNSIGNED;
            else if (strcmp(tok.str_val, "short") == 0) tok.type = TK_SHORT;
            else if (strcmp(tok.str_val, "long") == 0) tok.type = TK_LONG;
            else if (strcmp(tok.str_val, "auto") == 0) tok.type = TK_AUTO;
            else if (strcmp(tok.str_val, "_Generic") == 0) tok.type = TK_GENERIC;
            else if (strcmp(tok.str_val, "nullptr") == 0) tok.type = TK_NULLPTR;
            else if (strcmp(tok.str_val, "true") == 0) tok.type = TK_TRUE;
            else if (strcmp(tok.str_val, "false") == 0) tok.type = TK_FALSE;
            else if (strcmp(tok.str_val, "_Alignas") == 0) tok.type = TK_ALIGNAS;
            else if (strcmp(tok.str_val, "_Alignof") == 0) tok.type = TK_ALIGNOF;
            else if (strcmp(tok.str_val, "_Atomic") == 0) tok.type = TK_ATOMIC;
            else if (strcmp(tok.str_val, "_Noreturn") == 0) tok.type = TK_NORETURN;
            else if (strcmp(tok.str_val, "_Thread_local") == 0) tok.type = TK_THREAD_LOCAL;
            else if (strcmp(tok.str_val, "_Static_assert") == 0) tok.type = TK_STATIC_ASSERT;
            else if (strcmp(tok.str_val, "typeof") == 0) tok.type = TK_TYPEOF;
            else if (strcmp(tok.str_val, "typeof_unqual") == 0) tok.type = TK_TYPEOF_UNQUAL;
            else tok.type = TK_IDENT;

            c->tokens[c->token_count++] = tok;
            continue;
        }

        /* Preprocessor token operators: ## and # */
        if (s[0] == '#' && s[1] == '#') { tok.type = TK_PASTE; s += 2; }
        else if (*s == '#') { tok.type = TK_HASH; s++; }
        /* Two-character operators */
        if (s[0] == '+' && s[1] == '+') { tok.type = TK_INC; s += 2; }
        else if (s[0] == '-' && s[1] == '-') { tok.type = TK_DEC; s += 2; }
        else if (s[0] == '+' && s[1] == '=') { tok.type = TK_ADD_ASSIGN; s += 2; }
        else if (s[0] == '-' && s[1] == '=') { tok.type = TK_SUB_ASSIGN; s += 2; }
        else if (s[0] == '*' && s[1] == '=') { tok.type = TK_MUL_ASSIGN; s += 2; }
        else if (s[0] == '/' && s[1] == '=') { tok.type = TK_DIV_ASSIGN; s += 2; }
        else if (s[0] == '=' && s[1] == '=') { tok.type = TK_EQ; s += 2; }
        else if (s[0] == '!' && s[1] == '=') { tok.type = TK_NE; s += 2; }
        else if (s[0] == '<' && s[1] == '=') { tok.type = TK_LE; s += 2; }
        else if (s[0] == '>' && s[1] == '=') { tok.type = TK_GE; s += 2; }
        else if (s[0] == '&' && s[1] == '&') { tok.type = TK_LOGAND; s += 2; }
        else if (s[0] == '|' && s[1] == '|') { tok.type = TK_LOGOR; s += 2; }
        else if (s[0] == '<' && s[1] == '<') { tok.type = TK_SHL; s += 2; }
        else if (s[0] == '>' && s[1] == '>') { tok.type = TK_SHR; s += 2; }
        else if (s[0] == '-' && s[1] == '>') { tok.type = TK_ARROW; s += 2; }
        else if (s[0] == '.' && s[1] == '.' && s[2] == '.') { /* ellipsis */
            tok.type = TK_ELLIPSIS; zero_strlcpy(tok.str_val, "...", sizeof(tok.str_val)); s += 3;
        }
        /* Single-character tokens */
        else if (*s == ';') { tok.type = TK_SEMI; s++; }
        else if (*s == ':') { tok.type = TK_COLON; s++; }
        else if (*s == '{') { tok.type = TK_LBRACE; s++; }
        else if (*s == '}') { tok.type = TK_RBRACE; s++; }
        else if (*s == '(') { tok.type = TK_LPAREN; s++; }
        else if (*s == ')') { tok.type = TK_RPAREN; s++; }
        else if (*s == '[') { tok.type = TK_LBRACKET; s++; }
        else if (*s == ']') { tok.type = TK_RBRACKET; s++; }
        else if (*s == ',') { tok.type = TK_COMMA; s++; }
        else if (*s == '=') { tok.type = TK_ASSIGN; s++; }
        else if (*s == '+') { tok.type = TK_PLUS; s++; }
        else if (*s == '-') { tok.type = TK_MINUS; s++; }
        else if (*s == '*') { tok.type = TK_STAR; s++; }
        else if (*s == '/') { tok.type = TK_SLASH; s++; }
        else if (*s == '%') { tok.type = TK_PERCENT; s++; }
        else if (*s == '&') { tok.type = TK_AMP; s++; }
        else if (*s == '|') { tok.type = TK_PIPE; s++; }
        else if (*s == '^') { tok.type = TK_CARET; s++; }
        else if (*s == '~') { tok.type = TK_TILDE; s++; }
        else if (*s == '!') { tok.type = TK_BANG; s++; }
        else if (*s == '<') { tok.type = TK_LT; s++; }
        else if (*s == '>') { tok.type = TK_GT; s++; }
        else if (*s == '.') { tok.type = TK_DOT; s++; }
        else if (*s == '?') { tok.type = TK_QUESTION; s++; }
        else { /* Unknown/unsupported character: skip */ s++; continue; }

        c->tokens[c->token_count++] = tok;
    }

    ZeroCCTokenV2 eof_tok; memset(&eof_tok,0,sizeof(eof_tok)); eof_tok.type = TK_EOF; eof_tok.num_val = 0; eof_tok.str_val[0] = '\0';
    c->tokens[c->token_count++] = eof_tok;
}

/* Machine Code Emission Helpers (x86_64) */
static inline void zero_cc_emit_u8(ZeroCCBuffer *b, uint8_t byte) {
    zero_cc_buf_put_bytes(b, &byte, 1);
}
static inline void zero_cc_emit_u32(ZeroCCBuffer *b, uint32_t val) {
    zero_cc_buf_put_bytes(b, &val, 4);
}
static inline void zero_cc_emit_u64(ZeroCCBuffer *b, uint64_t val) {
    zero_cc_buf_put_bytes(b, &val, 8);
}

/* Abstraction layer: emit helpers that dispatch by target internally */
static inline void zero_cc_emit_prologue(ZeroCCCompiler *c, uint32_t stack_alloc) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)stack_alloc; /* TODO: implement ARM64 encoding */
#else
    zero_cc_emit_u8(&c->text_section, 0x55);
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x89);
    zero_cc_emit_u8(&c->text_section, 0xE5);
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x81);
    zero_cc_emit_u8(&c->text_section, 0xEC);
    zero_cc_emit_u32(&c->text_section, stack_alloc);
#endif
}

static inline void zero_cc_emit_epilogue(ZeroCCCompiler *c) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; /* TODO: implement ARM64 epilogue */
#else
    zero_cc_emit_u8(&c->text_section, 0xC9);
    zero_cc_emit_u8(&c->text_section, 0xC3);
#endif
}

static inline void zero_cc_emit_mov_imm64(ZeroCCCompiler *c, int reg_idx, uint64_t imm) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)reg_idx; (void)imm; /* TODO: implement ARM64 mov immediate sequences */
#else
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8 + (uint8_t)reg_idx);
    zero_cc_emit_u64(&c->text_section, imm);
#endif
}

static inline void zero_cc_emit_movss_rodata(ZeroCCCompiler *c, int xmm_idx, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)xmm_idx; (void)rodata_offset; /* TODO: implement AArch64 single-precision literal load (ldr s0, ...) */
#else
    /* movss xmmN, [rip + disp32] -> F3 0F 10 05 disp32 */
    (void)xmm_idx;
    zero_cc_emit_u8(&c->text_section, 0xF3);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x10);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}

static inline void zero_cc_emit_movsd_rodata(ZeroCCCompiler *c, int xmm_idx, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)xmm_idx; (void)rodata_offset; /* TODO: implement AArch64 double-precision literal load (ldr d0, ...) */
#else
    /* movsd xmmN, [rip + disp32] -> F2 0F 10 05 disp32 */
    (void)xmm_idx;
    zero_cc_emit_u8(&c->text_section, 0xF2);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x10);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}

static inline void zero_cc_emit_lea_rax_rodata(ZeroCCCompiler *c, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)rodata_offset; /* TODO: implement ADRP/ADD literal sequence for ARM64 */
#else
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x8D);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}

/* Symbol & Scope Management */
static inline ZeroCCSym* zero_cc_find_local(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->local_count; i++) {
        if (strcmp(c->locals[i].name, name) == 0) {
            return &c->locals[i];
        }
    }
    return NULL;
}

static inline ZeroCCSym* zero_cc_add_local(ZeroCCCompiler *c, const char *name, ZeroCCType type) {
    if (c->local_count >= ZERO_CC_MAX_LOCALS) return NULL;
    ZeroCCSym *sym = &c->locals[c->local_count++];
    snprintf(sym->name, sizeof(sym->name), "%s", name);
    sym->type = type;
    size_t alignment = type.align ? type.align : 8;
    if (alignment < 1) alignment = 8;
    if (decl_align_spec > 0) {
        if ((size_t)decl_align_spec > alignment) alignment = (size_t)decl_align_spec;
    }
    size_t alloc_sz = type.size ? type.size : alignment;
    if (alloc_sz < 4) alloc_sz = 4;
    c->current_stack_frame = (c->current_stack_frame + alignment - 1) & ~(alignment - 1);
    c->current_stack_frame += (int32_t)alloc_sz;
    sym->stack_offset = c->current_stack_frame;
    sym->is_param = false;
    sym->param_reg_idx = -1;
    decl_align_spec = 0;
    return sym;
}

static inline ZeroCCFunction* zero_cc_find_function(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->function_count; i++) {
        if (strcmp(c->functions[i].name, name) == 0) {
            return &c->functions[i];
        }
    }
    return NULL;
}

/* Dynamic Symbol Lookup for JIT Mode */
static inline void* zero_cc_resolve_symbol(const char *name) {
#ifdef __linux__
    if (strcmp(name, "stderr") == 0) return (void*)stderr;
    if (strcmp(name, "stdout") == 0) return (void*)stdout;
    if (strcmp(name, "stdin") == 0) return (void*)stdin;
    void *sym = dlsym(RTLD_DEFAULT, name);
    return sym;
#else
    (void)name;
    return NULL;
#endif
}

#endif /* ZERO_CC_TYPES_LEXER_H */

