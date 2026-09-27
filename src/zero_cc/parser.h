#ifndef ZERO_CC_PARSER_H
#define ZERO_CC_PARSER_H

#include <stdint.h>
#include <stddef.h>

typedef struct ZeroCCCompiler ZeroCCCompiler;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZeroCCCompiler ZeroCCCompiler;

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
    NODE_MEMBER,      /* left = struct_expr, is_arrow in flags, str_val = member */
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
    NODE_DECL,        /* var declaration: left = init, str_val = name */
#define NODE_FLAG_FUNC_PROTO  (1u << 0)  /* function prototype (no body, has param list) */
    NODE_TYPEDEF_DECL,/* struct/enum/scalar typedef */
    NODE_STATIC_ASSERT/* left = cond */
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

/* Contiguous Arena for AST Nodes */
typedef struct {
    ZeroCCASTNode *nodes;
    size_t count;
    size_t capacity;
} ZeroCCASTArena;

/* Arena Lifecycle */
void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap);
void zero_cc_ast_arena_free(ZeroCCASTArena *arena);
void zero_cc_ast_arena_reset(ZeroCCASTArena *arena);
ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag);
static inline ZeroCCASTNode *zero_cc_ast_get(ZeroCCASTArena *arena, ASTRef ref) {
    if (!arena || ref == AST_NULL || ref >= arena->count) return NULL;
    return &arena->nodes[ref];
}

/* Parser Functions using 32-bit AST Arena */
ASTRef zero_cc_parse_expr_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);
ASTRef zero_cc_parse_stmt_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena);

/* AST Pretty-Printer / Dumper for debugging and testing */
void zero_cc_ast_dump(ZeroCCASTArena *arena, ASTRef root, int indent);

/* AST Lowering to AMD64 Machine Code */
void zero_cc_lower_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena, ASTRef root);

#ifdef __cplusplus
}
#endif

#endif /* ZERO_CC_PARSER_H */
