#ifndef ZERO_CC_TYPES_TYPE_SYSTEM_H
#define ZERO_CC_TYPES_TYPE_SYSTEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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
    TYPE_UNION,
    TYPE_ENUM,
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
    bool is_flexible;  /* True for flexible array member (FAM) */
    bool is_bitfield;  /* True for bit-field member (: width) */
    uint8_t bit_offset;/* Bit offset within storage unit (0..63) */
    uint8_t bit_width; /* Bit-field width in bits (1..64) */
    bool is_signed;    /* Signed vs unsigned bit-field */
    bool is_const;
    bool is_volatile;
    bool is_atomic;
    bool is_complex;   /* C99 6.2.5: _Complex T member (base = element type) */
} ZeroCCStructMember;

typedef struct ZeroCCType {
    ZeroTypeTag tag;
    char struct_name[128];
    size_t size;
    size_t align;
    struct ZeroCCType *base;              /* For PTR and ARRAY */
    size_t array_len;                     /* For ARRAY */
    ZeroCCStructMember *members;     /* For STRUCT/UNION (heap; NULL when none) */
    size_t members_cap;              /* capacity of the heap members array */
    bool is_atomic;                       /* For _Atomic */
    bool is_const;
    bool is_volatile;
    bool is_restrict;
    bool is_vla;
    bool is_func_ptr;
    /* C99 long double: 16-byte object (x86-64 SysV) but lowered with the
     * 64-bit double value model. */
    bool is_long_double;
    /* C99 6.2.5: _Complex T - a two-part object of element type
     * `*base` (re-tagged), size 2 * element.  Lowering intercepts
     * arithmetic/comparisons and the __real__/__imag__ operators. */
    bool is_complex;
    /* Function type marker: the type denotes a function, not an object
     * (e.g. the alias in `typedef int f(void);`).  Applying a pointer
     * declarator on top of it yields a function pointer (is_func_ptr).
     * A function suffix applied to a base type sets this flag; a pointer
     * chunk then converts it. */
    bool is_func;

    size_t member_count;
} ZeroCCType;

/* Grow the (heap) member table of a struct/union type on demand (M3: the
 * fixed 64-member array silently dropped members beyond the cap). The array
 * is only mutated while the struct body is being parsed, before the type is
 * registered/shared, so all copies of a ZeroCCType may share it read-only. */
static inline bool zero_cc_type_reserve_members(ZeroCCType *t, size_t need) {
    if (need <= t->members_cap) return true;
    size_t old_cap = t->members_cap;
    size_t cap = old_cap ? old_cap : 8;
    while (cap < need) cap *= 2;
    ZeroCCStructMember *nm = (ZeroCCStructMember *)realloc(t->members, cap * sizeof(*nm));
    if (!nm) return false;
    /* Newly grown capacity must be zeroed: member write sites set fields
     * individually and rely on the rest (e.g. .base for scalars) being 0 -
     * the same invariant the old inline (zero-initialized) array provided. */
    if (cap > old_cap) memset(nm + old_cap, 0, (cap - old_cap) * sizeof(*nm));
    t->members = nm;
    t->members_cap = cap;
    return true;
}

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
    ZeroCCType type;
} ZeroCCEnumDef;

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
    bool is_vla;
    bool is_atomic;
    bool is_const;
    bool is_volatile;
    bool is_restrict;
    bool is_promoted;                     /* Parameter promoted to register (frameless leaf) */
    int promoted_vreg;                    /* Virtual register holding promoted parameter */
    /* Static local (Sprint 37): block-scoped declaration with static
     * storage duration. The object lives in the data section
     * (registered in the global table under static_global_name) and the
     * stack slot is a per-call mirror: the prologue copies
     * data -> slot, the epilogue (shared by all return paths) copies
     * slot -> data, so every access site stays frame-relative. */
    bool is_static_local;
    size_t static_data_offset;            /* Offset in data_section */
    char static_global_name[128];         /* Mangled name in the global table */
    bool is_extern;                       /* Block-scope extern declaration */
    /* C11 6.2.1: the block scope of this declaration has ended during
     * lowering (its enclosing NODE_BLOCK finished). The flat latest-wins
     * local table cannot express "out of scope" with the single numeric
     * local_lookup_bound - a shadowed name declared in a nested block
     * would otherwise be selected by outer-scope references before or
     * after the block. zero_cc_find_local skips scope-ended symbols. */
    bool scope_ended;
} ZeroCCSym;

/* Maximum parameters supported in a parsed function prototype/definition.
 * Conforms to C99/C11 translation limit (section 5.2.4.1 requires at least 127).
 * Parameters are stored as arena-allocated slices on ZeroCCFunction, so
 * unused capacity consumes zero memory. */
#define ZERO_CC_MAX_PARAMS 256

/* Function Definition Record */
typedef struct {
    char name[128];
    size_t text_offset;                   /* Byte offset in text_section */
    ZeroCCType return_type;
    size_t param_count;
    const char **param_names;             /* Arena-allocated array of param name strings */
    ZeroCCType *param_types;              /* Arena-allocated array of param types */
    bool returns_struct;
    size_t ret_struct_sz;
    int32_t sret_stack_offset;            /* Stack offset (positive, below rbp) for hidden sret ptr */
    bool is_variadic;
    bool noreturn;
    bool is_prototype;                    /* True for external prototypes (no body) */
    bool has_prototype;                   /* True when the declaration has a parameter
                                            * type list (incl. explicit (void)); empty-paren
                                            * () declarations are not prototypes (C99 6.7.5). */
    bool is_external;                     /* True for extern declarations */
    bool is_object;                       /* Extern OBJECT (variable) declaration
                                           * registered in the function table
                                           * (`extern int j;`): return_type holds
                                           * the variable type, and the global
                                           * scope must see it as a variable, not
                                           * a function. */
    bool is_inline;                       /* True for inline declarations (C99) */
    bool is_static;                       /* True for static functions (internal linkage) */
    size_t align;                         /* __attribute__((aligned(N))) on prototype/definition (0 = default) */
    bool can_inline;                      /* Eligible for small function inlining */
    uint32_t inline_body;                 /* ASTRef root of function body in compiler's tu_ast_arena */
    void *ast_arena;                      /* Pointer to ZeroCCASTArena owning inline_body */
    uint32_t *param_dim_exprs;    /* C99 6.7.6.2: VLA parameter dimension expressions (ASTRef),
                                     one slot per param (0 = none); evaluated in param
                                     order at function entry. */
    void *param_dim_arena;        /* ZeroCCASTArena* owning the param_dim_exprs nodes */
} ZeroCCFunction;

/* Relocation Record */
typedef struct {
    size_t text_patch_offset;
    char target_symbol[128];
    bool is_call;
    bool is_tls;        /* TLS access: AOT emits R_X86_64_TPOFF64 */
    size_t data_offset;  /* For data references (global variables) */
} ZeroCCReloc;

typedef struct {
    size_t text_patch_offset;
    size_t rodata_offset;
    int dst_reg;  /* Destination register for FP immediate loads */
} ZeroCCStringReloc;

/* Relocation for a pointer in .data that must be patched to address either
 * a byte range in .rodata (e.g. `const char *g = "abc";`) or another global
 * variable's storage (e.g. `int *p = &x;`). The 8 bytes at data_patch_offset
 * (within .data) receive the final absolute address.
 *   target_kind 0: .rodata + rodata_offset
 *   target_kind 1: .data + target_data_offset (global_vars[target_global_idx])
 *   target_kind 2: an object defined in ANOTHER translation unit, referenced
 *                  by name (target_extern_name). AOT: emitted as a .rela..
 *                  data relocation against the UND symbol; JIT: resolved
 *                  against the merged global-registry at patch time. */
typedef struct {
    size_t data_patch_offset;
    int    target_kind;
    size_t rodata_offset;
    size_t target_data_offset;
    int    target_global_idx;
    char   target_extern_name[128];
} ZeroCCDataReloc;


#endif /* ZERO_CC_TYPES_TYPE_SYSTEM_H */
