#ifndef ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H
#define ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/ast_expr.h"
#include "zero_cc/parser/ast_arena.h"
#include "zero_cc/jit/type_info.h"
#include "zero_cc/semantic/visitor.h"
#include "zero_cc/semantic/decl.h"

#define ZERO_CC_DECLARATOR_MAX_NESTING 16
#define ZERO_CC_DECLARATOR_MAX_LEVELS  16
#define ZERO_CC_DECLARATOR_MAX_ITEMS   16

typedef enum ZeroCCDeclChunkKind {
    ZERO_CC_DECL_CHUNK_POINTER,
    ZERO_CC_DECL_CHUNK_ARRAY,
    ZERO_CC_DECL_CHUNK_FUNCTION
} ZeroCCDeclChunkKind;

typedef struct ZeroCCDeclaratorChunk {
    ZeroCCDeclChunkKind kind;
    /* Qualifiers */
    bool is_const;
    bool is_volatile;
    bool is_restrict;
    bool is_atomic;
    /* Pointer */
    bool is_func_ptr;
    /* Array */
    size_t array_len;           /* 0 = VLA or unsized */
    bool is_vla;
    bool is_static_array;       /* [static N] */
    ASTRef dim_expr;
    /* Function */
    ASTRef param_list;          /* AST linked list of NODE_DECL */
    size_t param_count;
    TypeInfo *param_tis[16];
    bool has_prototype;         /* true if prototype list or (void), false if K&R () */
    bool is_variadic;
} ZeroCCDeclaratorChunk;

typedef struct ZeroCCDeclaratorLevel {
    ZeroCCDeclaratorChunk prefixes[ZERO_CC_DECLARATOR_MAX_ITEMS];
    size_t prefix_count;
    ZeroCCDeclaratorChunk suffixes[ZERO_CC_DECLARATOR_MAX_ITEMS];
    size_t suffix_count;
} ZeroCCDeclaratorLevel;

typedef struct ZeroCCDeclarator {
    ZeroCCDeclaratorLevel levels[ZERO_CC_DECLARATOR_MAX_LEVELS];
    size_t num_levels;

    /* Flattened stack: TypeObj stack[MAX_NESTING]; int top = 0; */
    ZeroCCDeclaratorChunk stack[ZERO_CC_DECLARATOR_MAX_NESTING];
    int top;

    /* Identifier */
    const char *name;
    size_t name_tok_pos;
    size_t name_level;
    bool is_abstract;
} ZeroCCDeclarator;

/* Forward declarations */
static ASTRef parse_expr_ast_internal(ZeroCCCompiler *c, ZeroCCASTArena *arena);
static bool is_type_specifier(ZeroCCCompiler *c, ZeroCCTokType t);
static inline ZeroCCType zero_cc_resolve_base_type(ZeroCCCompiler *c, ZeroCCTokType t, const char *s);

/* Initialize declarator struct */
static inline void zero_cc_declarator_init(ZeroCCDeclarator *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->num_levels = 1;
}

/* Parse parameter list of a function suffix: (type param, ...) */
static inline void zero_cc_parse_param_list(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx, ZeroCCDeclaratorChunk *fn_chunk)
{
    ASTRef param_head = AST_NULL;
    ASTRef param_tail = AST_NULL;
    bool first_param = true;
    while (c->token_pos < c->token_count) {
        if (c->tokens[c->token_pos].type == TK_RPAREN) {
            break;
        }
        if (!first_param) {
            if (c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
            } else {
                break;
            }
        }
        first_param = false;
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_ELLIPSIS) {
            fn_chunk->is_variadic = true;
            fn_chunk->has_prototype = true;
            c->token_pos++;
            break;
        }
        /* void alone means no params */
        if (c->tokens[c->token_pos].type == TK_VOID) {
            if (c->token_pos + 1 < c->token_count && c->tokens[c->token_pos + 1].type == TK_RPAREN) {
                fn_chunk->has_prototype = true;
                c->token_pos++;
                break;
            }
        }
        bool param_is_const = false;
        bool param_is_volatile = false;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
            c->token_pos++;
        }
        size_t param_vec_size = 0;
        {
            size_t save = c->token_pos;
            size_t probe = 0;
            if (zero_cc_parse_vector_attr(c, &c->token_pos, &probe)) {
                if (c->token_pos < c->token_count && is_type_specifier(c, c->tokens[c->token_pos].type)) {
                    param_vec_size = probe;
                } else {
                    c->token_pos = save;
                }
            }
        }
        size_t ptype_pos = c->token_pos;
        ZeroCCTokType param_type_tok = c->tokens[c->token_pos].type;
        if (!is_type_specifier(c, param_type_tok)) {
            break;
        }
        c->token_pos++;
        ZeroCCType param_typeof_type;
        memset(&param_typeof_type, 0, sizeof(param_typeof_type));
        bool param_has_typeof = false;
        if (param_type_tok == TK_TYPEOF || param_type_tok == TK_TYPEOF_UNQUAL) {
            param_has_typeof = true;
            bool unqual = (param_type_tok == TK_TYPEOF_UNQUAL);
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                c->token_pos++;
                if (zero_cc_tok_is_type_start(c, c->token_pos)) {
                    param_typeof_type = zero_cc_parse_type_name_at_pos(c, &c->token_pos);
                } else {
                    param_typeof_type = zero_cc_type_of_equality(c, &c->token_pos);
                }
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
                    c->token_pos++;
                }
            } else {
                param_typeof_type = zero_cc_type_int();
            }
            if (unqual) {
                param_typeof_type.is_const = false;
                param_typeof_type.is_volatile = false;
                param_typeof_type.is_restrict = false;
                param_typeof_type.is_atomic = false;
            }
        }
        bool param_is_unsigned = false;
        if (param_type_tok == TK_UNSIGNED || param_type_tok == TK_SIGNED) {
            param_is_unsigned = (param_type_tok == TK_UNSIGNED);
            if (c->token_pos < c->token_count &&
                (c->tokens[c->token_pos].type == TK_INT ||
                 c->tokens[c->token_pos].type == TK_CHAR ||
                 c->tokens[c->token_pos].type == TK_SHORT ||
                 c->tokens[c->token_pos].type == TK_LONG)) {
                ptype_pos = c->token_pos;
                param_type_tok = c->tokens[c->token_pos].type;
                c->token_pos++;
                if (param_type_tok == TK_SHORT && c->token_pos < c->token_count &&
                    c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                if (param_type_tok == TK_LONG) {
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
                    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
                }
            } else {
                param_type_tok = TK_INT;
            }
        } else if (param_type_tok == TK_LONG) {
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LONG) c->token_pos++;
            if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                param_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        } else if (param_type_tok == TK_SHORT) {
            if (c->token_pos < c->token_count && (c->tokens[c->token_pos].type == TK_UNSIGNED || c->tokens[c->token_pos].type == TK_SIGNED)) {
                param_is_unsigned = (c->tokens[c->token_pos].type == TK_UNSIGNED);
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_INT) c->token_pos++;
        }
        const char *param_tag_name = NULL;
        if ((param_type_tok == TK_STRUCT || param_type_tok == TK_UNION || param_type_tok == TK_ENUM) &&
            c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
            param_tag_name = c->tokens[c->token_pos].str_val;
            c->token_pos++;
        }
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT)) {
            if (c->tokens[c->token_pos].type == TK_CONST) param_is_const = true;
            if (c->tokens[c->token_pos].type == TK_VOLATILE) param_is_volatile = true;
            c->token_pos++;
        }
        const char *ptname = param_tag_name ? param_tag_name : c->tokens[ptype_pos].str_val;
        ZeroCCType param_base = param_has_typeof ? param_typeof_type : zero_cc_resolve_base_type(c, param_type_tok, ptname);
        if (param_is_unsigned) {
            if (param_base.tag == TYPE_INT8) param_base.tag = TYPE_UINT8;
            else if (param_base.tag == TYPE_INT16) param_base.tag = TYPE_UINT16;
            else if (param_base.tag == TYPE_INT32) param_base.tag = TYPE_UINT32;
            else if (param_base.tag == TYPE_INT64) param_base.tag = TYPE_UINT64;
        }
        param_base.is_const = param_is_const;
        param_base.is_volatile = param_is_volatile;
        /* GNU vector parameter: the marker probed above wraps the scalar
         * element type in a vector type (pointers, if any, apply on top). */
        if (param_vec_size > 0) {
            param_base = zero_cc_type_vector(&param_base, param_vec_size);
        }

        int param_ptr_depth = 0;
        ASTRef param_dim_expr = AST_NULL; /* VLA parameter dimension expr (C99 6.7.6.2) */
        bool param_ptr_is_const = false;
        bool param_ptr_is_volatile = false;
        bool param_ptr_is_restrict = false;
        bool is_param_func_ptr = false;

        const char *pname = NULL;
        if (c->token_pos + 1 < c->token_count &&
            c->tokens[c->token_pos].type == TK_LPAREN &&
            c->tokens[c->token_pos + 1].type == TK_STAR) {
            is_param_func_ptr = true;
            c->token_pos += 2;
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                pname = c->tokens[c->token_pos].str_val;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) c->token_pos++;
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LPAREN) {
                int pdepth = 1;
                c->token_pos++;
                while (c->token_pos < c->token_count && pdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LPAREN) pdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RPAREN) pdepth--;
                    c->token_pos++;
                }
            }
            param_ptr_depth = 1;
        } else {
            while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
                param_ptr_depth++;
                c->token_pos++;
                while (c->token_pos < c->token_count &&
                       (c->tokens[c->token_pos].type == TK_CONST ||
                        c->tokens[c->token_pos].type == TK_VOLATILE ||
                        c->tokens[c->token_pos].type == TK_RESTRICT)) {
                    if (c->tokens[c->token_pos].type == TK_CONST) param_ptr_is_const = true;
                    if (c->tokens[c->token_pos].type == TK_VOLATILE) param_ptr_is_volatile = true;
                    if (c->tokens[c->token_pos].type == TK_RESTRICT) param_ptr_is_restrict = true;
                    c->token_pos++;
                }
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT) {
                pname = c->tokens[c->token_pos].str_val;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_LBRACKET) {
                c->token_pos++;
                /* Array parameter: `int f(int a[n])` decays to `int *a`,
                 * but the dimension expression is evaluated at call entry
                 * (C99 6.7.6.2) - parse and keep it (VLA parameter
                 * dimension) instead of discarding the brackets. */
                size_t dim_start = c->token_pos;
                int bdepth = 1;
                while (c->token_pos < c->token_count && bdepth > 0) {
                    if (c->tokens[c->token_pos].type == TK_LBRACKET) bdepth++;
                    else if (c->tokens[c->token_pos].type == TK_RBRACKET) bdepth--;
                    c->token_pos++;
                }
                size_t dim_end = c->token_pos; /* just past ']' */
                if (dim_start + 1 < dim_end) {
                    size_t saved_pos = c->token_pos;
                    c->token_pos = dim_start;
                    param_dim_expr = parse_expr_ast_internal(c, arena);
                    c->token_pos = dim_end;
                    (void)saved_pos;
                }
                param_ptr_depth++;
            }
        }
        /* `(void)` is an empty parameter list (C99 6.7.6.3p2): it
         * makes the declaration a prototype but adds no parameters. */
        if (first_param && param_type_tok == TK_VOID && param_ptr_depth == 0 && (pname == NULL || pname[0] == '\0')) {
            fn_chunk->has_prototype = true;
            break;
        }
        if (pname == NULL) pname = "";

        ZeroCCType ptype = param_base;
        for (int pd = 0; pd < param_ptr_depth; pd++) {
            ptype = zero_cc_type_ptr(&ptype);
            if (pd == param_ptr_depth - 1) {
                ptype.is_const = param_ptr_is_const;
                ptype.is_volatile = param_ptr_is_volatile;
                ptype.is_restrict = param_ptr_is_restrict;
            }
        }
        if (is_param_func_ptr) {
            ptype.is_func_ptr = true;
        }

        ASTRef p_ref = zero_cc_ast_alloc_node(arena, NODE_DECL);
        ZeroCCASTNode *p_node = zero_cc_ast_get(arena, p_ref);
        p_node->line = c->tokens[c->token_pos].line;
        p_node->col = c->tokens[c->token_pos].col;
        p_node->s.str = pname;
        p_node->s_len = strlen(pname);
        p_node->right = param_dim_expr; /* VLA dimension, evaluated at entry */

        TypeInfo *ptype_ti = zero_cc_type_to_typeinfo(ctx->perm_arena, ptype);
        decl_node_set_type(ctx, p_node, ptype_ti);

        if (fn_chunk->param_count < 16) {
            fn_chunk->param_tis[fn_chunk->param_count++] = ptype_ti;
        }

        if (param_head == AST_NULL) {
            param_head = p_ref;
            param_tail = p_ref;
        } else {
            ZeroCCASTNode *tail_node = zero_cc_ast_get(arena, param_tail);
            if (tail_node) tail_node->next = p_ref;
            param_tail = p_ref;
        }
    }
    if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
        c->token_pos++;
    }
    fn_chunk->param_list = param_head;
    if (!first_param) {
        fn_chunk->has_prototype = true;
    }
}

/* Recursive declarator parse */
static inline bool zero_cc_parse_declarator_internal(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx,
    ZeroCCDeclarator *d, size_t level)
{
    if (level >= ZERO_CC_DECLARATOR_MAX_LEVELS) {
        diag_report_error_at_token(c, "declarator nesting depth exceeds limit");
        return false;
    }
    if (level >= d->num_levels) {
        d->num_levels = level + 1;
    }
    ZeroCCDeclaratorLevel *lvl = &d->levels[level];

    /* 1. Parse pointer asterisks and qualifiers at this level */
    while (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_STAR) {
        c->token_pos++;
        ZeroCCDeclaratorChunk ptr_chunk;
        memset(&ptr_chunk, 0, sizeof(ptr_chunk));
        ptr_chunk.kind = ZERO_CC_DECL_CHUNK_POINTER;
        while (c->token_pos < c->token_count &&
               (c->tokens[c->token_pos].type == TK_CONST ||
                c->tokens[c->token_pos].type == TK_VOLATILE ||
                c->tokens[c->token_pos].type == TK_RESTRICT ||
                c->tokens[c->token_pos].type == TK_ATOMIC)) {
            if (c->tokens[c->token_pos].type == TK_CONST) ptr_chunk.is_const = true;
            else if (c->tokens[c->token_pos].type == TK_VOLATILE) ptr_chunk.is_volatile = true;
            else if (c->tokens[c->token_pos].type == TK_RESTRICT) ptr_chunk.is_restrict = true;
            else if (c->tokens[c->token_pos].type == TK_ATOMIC) ptr_chunk.is_atomic = true;
            c->token_pos++;
        }
        /* GNU aligned(N) marker between pointer chunks:
         * `int *__attribute__((aligned(16))) *p;` - alignment applies to
         * the object, not the pointer; consume and discard here (the
         * declaration-level probes already handled the usual positions). */
        {
            size_t probe = 0;
            while (zero_cc_parse_aligned_attr(c, &c->token_pos, &probe)) { /* skip */ }
        }
        if (lvl->prefix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
            lvl->prefixes[lvl->prefix_count++] = ptr_chunk;
        }
    }

    /* 2. Direct declarator: parenthesized declarator, identifier, or abstract */
    bool is_paren = false;
    if (d->name == NULL && c->token_pos < c->token_count &&
        c->tokens[c->token_pos].type == TK_LPAREN) {
        if (c->token_pos + 1 < c->token_count) {
            ZeroCCTokType next_tok = c->tokens[c->token_pos + 1].type;
            if (next_tok == TK_STAR || next_tok == TK_LPAREN) {
                is_paren = true;
            } else if (next_tok == TK_IDENT) {
                const char *s = c->tokens[c->token_pos + 1].str_val;
                if (!zero_cc_find_typedef(c, s)) {
                    is_paren = true;
                }
            } else if (next_tok == TK_RPAREN) {
                is_paren = false;
            }
        }
    }

    if (is_paren) {
        c->token_pos++; /* consume '(' */
        if (!zero_cc_parse_declarator_internal(c, arena, ctx, d, level + 1)) {
            return false;
        }
        if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RPAREN) {
            c->token_pos++; /* consume ')' */
        } else {
            diag_report_error_at_token(c, "expected ')' closing parenthesized declarator");
            return false;
        }
    } else if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_IDENT && d->name == NULL) {
        ZeroCCToken *tok = &c->tokens[c->token_pos];
        d->name = tok->allocated_str ? tok->allocated_str : tok->str_val;
        d->name_tok_pos = c->token_pos;
        d->name_level = level;
        c->token_pos++;
    } else {
        d->is_abstract = true;
    }

    /* 3. Suffixes (arrays and functions) at this level */
    while (c->token_pos < c->token_count) {
        if (c->tokens[c->token_pos].type == TK_LBRACKET) {
            c->token_pos++; /* consume '[' */
            ZeroCCDeclaratorChunk arr_chunk;
            memset(&arr_chunk, 0, sizeof(arr_chunk));
            arr_chunk.kind = ZERO_CC_DECL_CHUNK_ARRAY;
            while (c->token_pos < c->token_count &&
                   (c->tokens[c->token_pos].type == TK_CONST ||
                    c->tokens[c->token_pos].type == TK_VOLATILE ||
                    c->tokens[c->token_pos].type == TK_RESTRICT ||
                    c->tokens[c->token_pos].type == TK_STATIC)) {
                if (c->tokens[c->token_pos].type == TK_CONST) arr_chunk.is_const = true;
                else if (c->tokens[c->token_pos].type == TK_VOLATILE) arr_chunk.is_volatile = true;
                else if (c->tokens[c->token_pos].type == TK_RESTRICT) arr_chunk.is_restrict = true;
                else if (c->tokens[c->token_pos].type == TK_STATIC) arr_chunk.is_static_array = true;
                c->token_pos++;
            }
            if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                c->token_pos++;
                arr_chunk.array_len = 0;
            } else {
                ASTRef dim_expr = parse_expr_ast_internal(c, arena);
                if (c->token_pos < c->token_count && c->tokens[c->token_pos].type == TK_RBRACKET) {
                    c->token_pos++;
                } else {
                    diag_report_error_at_token(c, "expected ']' after array dimension");
                    return false;
                }
                ZeroCCASTNode *dim_node = zero_cc_ast_get(arena, dim_expr);
                if (dim_node && dim_node->tag == NODE_INT && dim_node->int_val >= 0) {
                    arr_chunk.array_len = (size_t)dim_node->int_val;
                } else {
                    arr_chunk.is_vla = true;
                    arr_chunk.dim_expr = dim_expr;
                }
            }
            if (lvl->suffix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
                lvl->suffixes[lvl->suffix_count++] = arr_chunk;
            }
        } else if (c->tokens[c->token_pos].type == TK_LPAREN) {
            c->token_pos++; /* consume '(' */
            ZeroCCDeclaratorChunk fn_chunk;
            memset(&fn_chunk, 0, sizeof(fn_chunk));
            fn_chunk.kind = ZERO_CC_DECL_CHUNK_FUNCTION;
            zero_cc_parse_param_list(c, arena, ctx, &fn_chunk);
            if (lvl->suffix_count < ZERO_CC_DECLARATOR_MAX_ITEMS) {
                lvl->suffixes[lvl->suffix_count++] = fn_chunk;
            }
        } else {
            break;
        }
    }
    return true;
}

/* Flatten the hierarchy into DeclaratorChunk stack[MAX_NESTING]; int top = 0;
 * Order: inner level before outer level; within a level: suffixes forward, prefixes reverse. */
static inline void zero_cc_declarator_flatten(ZeroCCDeclarator *d) {
    d->top = 0;
    for (size_t lvl = d->num_levels; lvl > 0; lvl--) {
        size_t l = lvl - 1;
        /* Suffixes forward */
        for (size_t s = 0; s < d->levels[l].suffix_count; s++) {
            if (d->top < ZERO_CC_DECLARATOR_MAX_NESTING) {
                d->stack[d->top++] = d->levels[l].suffixes[s];
            }
        }
        /* Prefixes reverse (innermost pointer first) */
        for (size_t p = d->levels[l].prefix_count; p > 0; p--) {
            if (d->top < ZERO_CC_DECLARATOR_MAX_NESTING) {
                d->stack[d->top++] = d->levels[l].prefixes[p - 1];
            }
        }
    }
    /* Mark function pointer modifiers */
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_POINTER) {
            if (i + 1 < d->top && d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                d->stack[i].is_func_ptr = true;
            }
        }
    }
}

/* Parse a full declarator starting from level 0 */
static inline bool zero_cc_parse_declarator(
    ZeroCCCompiler *c, ZeroCCASTArena *arena, SemContext *ctx, ZeroCCDeclarator *d)
{
    zero_cc_declarator_init(d);
    if (!zero_cc_parse_declarator_internal(c, arena, ctx, d, 0)) {
        return false;
    }
    zero_cc_declarator_flatten(d);
    return true;
}

/* Check if declarator is a function declaration/definition */
static inline bool zero_cc_declarator_is_function(const ZeroCCDeclarator *d) {
    if (!d || d->name == NULL) return false;
    size_t nl = d->name_level;
    if (nl < d->num_levels && d->levels[nl].suffix_count > 0) {
        return d->levels[nl].suffixes[0].kind == ZERO_CC_DECL_CHUNK_FUNCTION;
    }
    return false;
}

/* Get the primary function chunk corresponding to a function declarator */
static inline const ZeroCCDeclaratorChunk *zero_cc_declarator_get_function_chunk(const ZeroCCDeclarator *d) {
    if (!d || d->name == NULL) return NULL;
    size_t nl = d->name_level;
    if (nl < d->num_levels && d->levels[nl].suffix_count > 0) {
        if (d->levels[nl].suffixes[0].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
            for (int i = 0; i < d->top; i++) {
                if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_FUNCTION &&
                    d->stack[i].param_list == d->levels[nl].suffixes[0].param_list) {
                    return &d->stack[i];
                }
            }
            return &d->levels[nl].suffixes[0];
        }
    }
    return NULL;
}

/* Check if declarator has any array modifier */
static inline bool zero_cc_declarator_has_array(const ZeroCCDeclarator *d) {
    if (!d) return false;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY) return true;
    }
    return false;
}

/* Check if declarator has any VLA modifier */
static inline bool zero_cc_declarator_has_vla(const ZeroCCDeclarator *d) {
    if (!d) return false;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY && d->stack[i].is_vla) return true;
    }
    return false;
}

/* Return the outermost VLA's dimension AST expression */
static inline ASTRef zero_cc_declarator_vla_dim_expr(const ZeroCCDeclarator *d) {
    if (!d) return AST_NULL;
    for (int i = 0; i < d->top; i++) {
        if (d->stack[i].kind == ZERO_CC_DECL_CHUNK_ARRAY && d->stack[i].is_vla) {
            return d->stack[i].dim_expr;
        }
    }
    return AST_NULL;
}

/* Validate declarator against standard C constraints */
static inline bool zero_cc_declarator_validate(ZeroCCCompiler *c, const ZeroCCDeclarator *d) {
    if (!d) return false;
    bool seen_outer_vla = false;
    for (int i = 0; i < d->top; i++) {
        const ZeroCCDeclaratorChunk *chunk = &d->stack[i];
        if (chunk->kind == ZERO_CC_DECL_CHUNK_ARRAY) {
            if (chunk->is_vla) {
                if (seen_outer_vla) {
                    diag_report_error_at_token(c, "multi-dim VLA with runtime inner dimensions is not supported");
                    return false;
                }
                seen_outer_vla = true;
            }
            /* C99 6.7.5.2: array of functions is invalid */
            if (i + 1 < d->top && d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                diag_report_error_at_token(c, "declaration of array of functions is not allowed");
                return false;
            }
        } else if (chunk->kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
            /* C99 6.7.5.3: function returning function or array is invalid */
            if (i + 1 < d->top) {
                if (d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
                    diag_report_error_at_token(c, "function cannot return a function");
                    return false;
                }
                if (d->stack[i + 1].kind == ZERO_CC_DECL_CHUNK_ARRAY) {
                    diag_report_error_at_token(c, "function cannot return an array");
                    return false;
                }
            }
        }
    }
    return true;
}

/* Apply a single DeclaratorChunk modifier to a base/target type */
static inline ZeroCCType zero_cc_chunk_apply(const ZeroCCDeclaratorChunk *chunk, ZeroCCType target) {
    if (chunk->kind == ZERO_CC_DECL_CHUNK_POINTER) {
        ZeroCCType p = zero_cc_type_ptr(&target);
        p.is_const = chunk->is_const;
        p.is_volatile = chunk->is_volatile;
        p.is_restrict = chunk->is_restrict;
        p.is_atomic = chunk->is_atomic;
        /* A pointer whose pointee is (or became) a function type is a
         * function pointer: `int (*fp)(void)` and `func *fp` (where
         * `typedef int func(void);`) both need is_func_ptr set. */
        p.is_func_ptr = chunk->is_func_ptr || target.is_func;
        return p;
    }
    if (chunk->kind == ZERO_CC_DECL_CHUNK_ARRAY) {
        ZeroCCType arr;
        if (chunk->is_vla) {
            arr = zero_cc_type_array(&target, 0);
            arr.is_vla = true;
            arr.size = 8;
            arr.align = 8;
        } else {
            arr = zero_cc_type_array(&target, (ssize_t)chunk->array_len);
        }
        arr.is_const = chunk->is_const;
        arr.is_volatile = chunk->is_volatile;
        return arr;
    }
    if (chunk->kind == ZERO_CC_DECL_CHUNK_FUNCTION) {
        /* A function suffix makes the type a function type.  When this
         * chunk is not the name's own (it is skipped in apply_return),
         * it builds an outer function type, e.g. `void (*f(void))(void)`
         * — a function returning a function pointer: the outer suffix
         * marks is_func and the inner pointer chunk converts it. */
        target.is_func = true;
        return target;
    }
    return target;
}

/* Build the complete variable type:
 * for i = top-1..0: apply DeclaratorChunk[i] to base_type */
static inline ZeroCCType zero_cc_declarator_apply(
    const ZeroCCDeclarator *d, ZeroCCType base_type)
{
    ZeroCCType cur = base_type;
    for (int i = d->top - 1; i >= 0; i--) {
        cur = zero_cc_chunk_apply(&d->stack[i], cur);
    }
    return cur;
}

/* Build the return type for a function declarator (skipping its primary function chunk) */
static inline ZeroCCType zero_cc_declarator_apply_return(
    const ZeroCCDeclarator *d, ZeroCCType base_type)
{
    ZeroCCType cur = base_type;
    const ZeroCCDeclaratorChunk *fn_chunk = zero_cc_declarator_get_function_chunk(d);
    for (int i = d->top - 1; i >= 0; i--) {
        if (&d->stack[i] == fn_chunk) continue;
        cur = zero_cc_chunk_apply(&d->stack[i], cur);
    }
    return cur;
}

#endif /* ZERO_CC_PARSER_ZERO_CC_DECLARATOR_STACK_H */
