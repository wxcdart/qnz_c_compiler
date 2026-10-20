#ifndef ZERO_CC_PARSER_H
#define ZERO_CC_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;
typedef struct SemContext SemContext;

#ifdef __cplusplus
extern "C" {
#endif

/* 32-bit thin pointer into the AST Arena */
typedef uint32_t ASTRef;
#define AST_NULL 0

/* AST Node Types */
typedef enum {
    NODE_NONE = 0,
    /* Literals & Primary */
    NODE_INT,
    NODE_FLOAT,
    NODE_STRING,
    NODE_IDENT,
    NODE_NULLPTR,
    NODE_BOOL,
    /* Operators */
    NODE_UNOP,        /* op in aux.op, left = operand */
    NODE_BINOP,       /* op in aux.op, left = lhs, right = rhs */
    NODE_ASSIGN,      /* op in aux.op (=, +=, etc.), left = target, right = val */
    NODE_TERNARY,     /* left = cond, right = then_expr, aux.alt = else_expr */
    NODE_CAST,        /* left = expr, aux.type_ref */
    NODE_MEMBER,      /* left = struct_expr, flags = NODE_MEMBER_*, s.str = member */
    /* Flags for NODE_MEMBER: */
#define NODE_MEMBER_IS_ARROW    (1u << 0)
#define NODE_MEMBER_IS_BITFIELD (1u << 1)
#define NODE_FLAG_DESIGNATED    (1u << 15)  /* element has explicit designator (.field or [idx]) */
    NODE_VA_START,    /* left = ap expr, right = last expr (for computing spill area) */
    NODE_VA_ARG,      /* left = va_list expr, aux.type_ref = target type */
    NODE_INDEX,       /* left = array_expr, right = index_expr */
    NODE_CALL,        /* left = callee, right = arg_list */
    NODE_COMPOUND_LIT,/* left = init_list */
    /* Statements */
    NODE_BLOCK,       /* left = first_stmt (chained via next) */
    NODE_EXPR_STMT,   /* left = expr */
    NODE_IF,          /* left = cond, right = then_stmt, aux.alt = else_stmt */
    NODE_WHILE,       /* left = cond, right = body */
    NODE_DO_WHILE,    /* left = cond, right = body */
    NODE_FOR,         /* left = init, right = cond, aux.step, aux.body */
    NODE_SWITCH,      /* left = expr, right = body */
    NODE_CASE,        /* left = const_expr, right = stmt */
    NODE_DEFAULT,     /* left = stmt */
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_GOTO,        /* s.str = label name */
    NODE_LABEL,       /* s.str = label name, left = stmt */
    NODE_RETURN,      /* left = expr (optional) */
    NODE_DECL,        /* var / func declaration: left = init, str_val = name */
    /* Flags for NODE_DECL: */
#define NODE_DECL_FUNC_PROTO  (1u << 0)  /* function prototype (no body, has param list) */
#define NODE_FLAG_FUNC_PROTO  NODE_DECL_FUNC_PROTO /* alias for backwards-compat */
#define NODE_DECL_IS_VLA      (1u << 1)  /* variable length array declaration */
    NODE_TYPEDEF_DECL,/* struct/enum/scalar typedef */
    NODE_STATIC_ASSERT/* left = cond */,
    NODE_COMMA,       /* left = lhs, right = rhs (comma operator) */
} ZeroCCASTTag;

/* Cache-friendly, 32-bit thin-pointer AST node (32 bytes total) */
typedef struct ZeroCCASTNode {
    uint16_t tag;         /* ZeroCCASTTag */
    uint16_t flags;       /* is_const, is_arrow, is_atomic, etc. */
    uint32_t line;        /* 1-based source line */
    uint32_t col;         /* 1-based source column */

    /* 32-bit arena indices (thin pointers) */
    ASTRef left;
    ASTRef right;
    ASTRef next;          /* next statement in block or next arg in call list */
    uint32_t desig_idx;   /* member/element index for NODE_FLAG_DESIGNATED initializer elements.
                           * Kept out of the payload union: aux.alt aliases the high half of
                           * s.str / int_val, so storing it there corrupted string pointers
                           * and 64-bit constants of designated initializers. */
    const char *member_name; /* NODE_MEMBER: field name saved by semantic analysis before it
                              * stores the offset in aux.alt (which overwrites the high half of
                              * s.str). Lets a node be analyzed more than once. */

    /* Payload */
    union {
        int64_t  int_val;
        double   float_val;
        struct {
            const char *str;  /* pointer into token/source, zero-copy, unlimited size */
            size_t      len;
        } s;
        struct {
            uint32_t op;      /* operator token type */
            uint32_t alt;     /* secondary branch (e.g. else_stmt in if) */
            uint32_t step;    /* step in for loop */
            uint32_t extra;   /* extra metadata / type id */
        } aux;
    };
} ZeroCCASTNode;

#define ZERO_CC_AST_CHUNK_BITS 10
#define ZERO_CC_AST_CHUNK_SIZE (1 << ZERO_CC_AST_CHUNK_BITS) /* 1024 nodes per chunk */
#define ZERO_CC_AST_CHUNK_MASK (ZERO_CC_AST_CHUNK_SIZE - 1)

/* Segmented Chunk Arena for AST Nodes: pointers never dangle on growth */
typedef struct {
    ZeroCCASTNode **chunks;
    size_t chunk_count;
    size_t chunk_capacity;
    size_t count;
} ZeroCCASTArena;

/* Arena Lifecycle */
void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap);
void zero_cc_ast_arena_free(ZeroCCASTArena *arena);
void zero_cc_ast_arena_reset(ZeroCCASTArena *arena);
ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag);
size_t zero_cc_ast_node_count(ZeroCCASTArena *arena, ASTRef root);
/* Compile-time size of the AST node; the caller (main) compares it against
 * its own sizeof(ZeroCCASTNode) to catch stale-build objects. */
size_t zero_cc_ast_node_size(void);
ASTRef zero_cc_ast_clone(ZeroCCASTArena *src_arena, ASTRef root, ZeroCCASTArena *dst_arena);
bool zero_cc_ast_has_call(ZeroCCASTArena *arena, ASTRef root, const char *target_name);
bool zero_cc_ast_has_local_decl(ZeroCCASTArena *arena, ASTRef root);
bool zero_cc_ast_has_label_goto(ZeroCCASTArena *arena, ASTRef root);
bool zero_cc_ast_addr_of_param(ZeroCCASTArena *arena, ASTRef root,
                               const char *param_names[], int param_count);
static inline ZeroCCASTNode *zero_cc_ast_get(ZeroCCASTArena *arena, ASTRef ref) {
    if (!arena || ref == AST_NULL || ref >= arena->count) return NULL;
    size_t chunk_idx = ref >> ZERO_CC_AST_CHUNK_BITS;
    size_t node_idx  = ref & ZERO_CC_AST_CHUNK_MASK;
    return &arena->chunks[chunk_idx][node_idx];
}

/* Parser Functions using 32-bit AST Arena */
ASTRef zero_cc_parse_expr_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx);

/* AST Pretty-Printer / Dumper for debugging and testing */
void zero_cc_ast_dump(ZeroCCASTArena *arena, ASTRef root, int indent);

/* AST Lowering to AMD64 Machine Code */
void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_PARSER_H */
