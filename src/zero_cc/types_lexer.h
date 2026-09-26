/* --------------------------------------------------------------------------
 *  3. STEP 2 & 3: ADVANCED C99 TYPE SYSTEM & COMPILER ENGINE
 *  -------------------------------------------------------------------------- */

#include "core.h"
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
#include "zero_cc_backends.h"
#ifndef ZERO_CC_TYPES_LEXER_H
#define ZERO_CC_TYPES_LEXER_H

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
    bool is_const;
    bool is_volatile;

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
typedef struct ZeroCCCompiler {
    ZeroCCToken *tokens;
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
    // Thread‑local (static alias) storage – per compiler instance
    struct {
        char name[128];
        int64_t value;
        ZeroCCType type;
    } thread_locals[64];
    size_t thread_local_count;
    /* Backend pointers */
    const ZeroCCTLSBackend *tls_backend;
    const ZeroCCEmitBackend   *emit_backend;
    const ZeroCCLinkBackend   *link_backend;
    const ZeroCCMemoryBackend *memory_backend;
    const ZeroCCABICallBackend *abi_backend;
    const ZeroCCAtomicBackend *atomic_backend;
    const ZeroCCGenericBackend *generic_backend;
    const ZeroCCStdlibBackend *stdlib_backend;
    const ZeroCCDiagBackend   *diag_backend;
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
    if (base) {
        ZeroCCType *heap_base = (ZeroCCType *)malloc(sizeof(ZeroCCType));
        *heap_base = *base;
        t.base = heap_base;
    } else {
        t.base = NULL;
    }
    return t;
}

static inline ZeroCCType zero_cc_type_array(ZeroCCType *base, ssize_t length) {
    ZeroCCType t; memset(&t, 0, sizeof(t)); t.tag = TYPE_ARRAY; t.base = base; t.array_len = length; if (length > 0) { t.size = base->size * length; } else { t.size = 0; } t.align = base->align; return t;
}

static inline ZeroCCType zero_cc_type_strip_qualifiers(ZeroCCType ty) {
    ty.is_const = false;
    ty.is_volatile = false;
    ty.is_atomic = false;
    return ty;
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

/* Lexer implementation (moved to lexer.c/lexer.h) */
void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c);
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len);
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

/* Find a thread‑local variable by name. Returns the opaque handle used by the TLS backend, or NULL if not found. */
static inline void *zero_cc_find_tls(ZeroCCCompiler *c, const char *name) {
    for (size_t i = 0; i < c->thread_local_count; i++) {
        if (strcmp(c->thread_locals[i].name, name) == 0) {
            return &c->thread_locals[i];
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

static inline int zero_cc_types_compatible(ZeroCCType a, ZeroCCType b) {
    a = zero_cc_type_strip_qualifiers(a);
    b = zero_cc_type_strip_qualifiers(b);
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_NULLPTR:
            return 1;
        case TYPE_PTR:
            /* If either base pointer is NULL or TYPE_VOID, treat as compatible with any object pointer */
            if (!a.base || !b.base || a.base->tag == TYPE_VOID || b.base->tag == TYPE_VOID) {
                return 1;
            }
            return zero_cc_types_compatible(*a.base, *b.base);
        case TYPE_ARRAY:
            if (!a.base || !b.base) return 1;
            return zero_cc_types_compatible(*a.base, *b.base) && a.array_len == b.array_len;
        case TYPE_STRUCT:
            return strcmp(a.struct_name, b.struct_name) == 0;
        default:
            return 0;
    }
}

#endif /* ZERO_CC_TYPES_LEXER_H */

