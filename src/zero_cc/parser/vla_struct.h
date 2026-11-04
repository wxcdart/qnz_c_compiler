#ifndef ZERO_CC_PARSER_VLA_STRUCT_H
#define ZERO_CC_PARSER_VLA_STRUCT_H

/* GNU variably-modified struct support (C11 6.7.2.1p18 extension, as
 * accepted by GCC -std=gnuXX): a block-scope struct/union may have an
 * array member whose dimension is a runtime expression:
 *
 *     struct s { int a; char b[argc]; };
 *     struct s t;                 // runtime-sized object
 *     int f(struct s x) { ... }   // param: (hidden ptr, size) ABI
 *     sizeof(t) / sizeof(x)       // runtime value
 *
 * Layout model (matches GCC):
 *  - STATIC layout: a VLA member contributes a 2-element placeholder, so
 *    every member gets a compile-time offset and the struct has a static
 *    size (ZeroCCType.size). Member access uses these static offsets.
 *  - RUNTIME object size: the members are re-laid out with the REAL
 *    dimension (each member aligned as usual, VLA member contributes
 *    dim * elem_size). This is what allocation, sizeof, and the
 *    by-value-parameter size word use. The re-layout is NOT a linear
 *    delta of the static size - alignment absorbs the difference when
 *    the dimension crosses an alignment boundary (sizeof of
 *    {int;char[N]} with N=4 is 8, not 10).
 *
 * Storage:
 *  - Locals: allocated with OP_VLA_ALLOC like VLA array locals; the frame
 *    slot holds the base pointer, the vla_slot_off slot holds the exact
 *    runtime size (so runtime sizeof works).
 *  - By-value parameters / call arguments: two consecutive GPR units -
 *    the object pointer, then the total runtime size. The callee stores
 *    the pointer in the parameter slot (addressed through
 *    sym->is_vla_struct) and the size in its reserved VLA slot.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"

/* Follow the (constant) array chain of a declared object type and return
 * the struct/union type carrying VLA members, or NULL. Stops at pointer
 * boundaries (a pointer to a VLA struct is just an 8-byte object). */
static inline const ZeroCCType *zc_find_vla_struct(const ZeroCCType *t) {
    for (const ZeroCCType *tt = t; tt; tt = tt->base) {
        if (tt->tag == TYPE_PTR) break;
        if (tt->tag == TYPE_ARRAY) {
            if (!tt->base) break;
            continue;
        }
        if ((tt->tag == TYPE_STRUCT || tt->tag == TYPE_UNION) && tt->is_vla_struct)
            return tt;
        break;
    }
    return NULL;
}

/* True when a declared object (or its constant-array element) is a
 * struct/union with VLA members - i.e. its storage is runtime-sized. */
static inline bool zc_type_is_vla_struct_obj(const ZeroCCType *t) {
    return zc_find_vla_struct(t) != NULL;
}

static inline ASTRef zc_vla_size_const(ZeroCCASTArena *a, int64_t v) {
    ASTRef r = zero_cc_ast_alloc_node(a, NODE_INT);
    ZeroCCASTNode *n = zero_cc_ast_get(a, r);
    n->int_val = v;
    return r;
}

static inline ASTRef zc_vla_size_binop(ZeroCCASTArena *a, ASTRef l, ASTRef r, uint32_t op) {
    ASTRef ref = zero_cc_ast_alloc_node(a, NODE_BINOP);
    ZeroCCASTNode *n = zero_cc_ast_get(a, ref);
    n->left = l;
    n->right = r;
    n->aux.op = op;
    return ref;
}

/* (x + a - 1) & ~(a - 1) - runtime align-up (a is a power of two).
 * Note: ~(a-1) as a two's-complement constant is -a (not -(a-1)). */
static inline ASTRef zc_vla_size_align_up(ZeroCCASTArena *a, ASTRef x, size_t al) {
    if (al <= 1) return x;
    return zc_vla_size_binop(a,
                             zc_vla_size_binop(a, x, zc_vla_size_const(a, (int64_t)al - 1), TK_PLUS),
                             zc_vla_size_const(a, -(int64_t)al), TK_AMP);
}

/* Build an AST expression for the RUNTIME total size of a VLA struct
 * type: the member walk with the real dimensions (see the file header).
 * Constant segments are folded; each VLA member injects
 * dim_expr * elem_size, and a struct member that is itself a VLA struct
 * (or an array of them) recurses. Returns AST_NULL when the type has no
 * VLA member (callers should not then need a runtime size).
 *
 * The dimension expressions were captured in `dim_arena` (the TU arena)
 * at the struct definition. Instead of re-evaluating them at every use
 * site, the size expression embeds NODE_VLA_DIM_SLOT nodes: the
 * dimension values are captured ONCE when the struct definition executes
 * (GCC semantics - a `n++; sizeof(struct s)` reads the value at the
 * definition, not at the sizeof) and stored in per-function frame slots
 * (c->lowering.vla_struct_captures). A use site in a different function
 * (e.g. a nested function) falls back to evaluating the original dim
 * expression (node->left, cloned on demand). */
static inline ASTRef zc_vla_struct_size_ast(ZeroCCASTArena *arena, ZeroCCASTArena *dim_arena, const ZeroCCType *st, const char *tag) {
    (void)dim_arena;
    if (!st || !st->is_vla_struct || !st->members) return AST_NULL;

    /* Index of the VLA member among the struct's VLA members (member
     * order) - matches the capture slot index at the definition site. */
    uint32_t vla_dim_idx = 0;

    /* Accumulator: `cval` tracks the value while the tree is still a
     * pure constant; `tree` is materialized as soon as a runtime piece
     * (a VLA dimension) is injected. */
    int64_t cval = 0;
    bool have_tree = false;
    ASTRef tree = AST_NULL;

    for (size_t i = 0; i < st->member_count && st->members; i++) {
        const ZeroCCStructMember *m = &st->members[i];
        /* Align the running offset to the member's alignment. */
        if (m->align > 1) {
            if (!have_tree) {
                cval = (cval + (int64_t)m->align - 1) & ~((int64_t)m->align - 1);
            } else {
                tree = zc_vla_size_align_up(arena, tree, m->align);
                cval = 0;
            }
        }
        if (m->is_vla) {
            if (m->vla_dim == 0) continue;
            if (!have_tree) {
                tree = zc_vla_size_const(arena, cval);
                have_tree = true;
                cval = 0;
            }
            ASTRef piece = zero_cc_ast_alloc_node(arena, NODE_VLA_DIM_SLOT);
            ZeroCCASTNode *pn = zero_cc_ast_get(arena, piece);
            /* tag must be PERSISTENT (the registered struct's name or an
             * interned string): the node outlives the caller's stack. */
            pn->s.str = tag && tag[0] ? tag : st->struct_name;
            pn->s_len = strlen(pn->s.str);
            /* desig_idx is the only non-union-aliased 32-bit slot -
             * aux.alt shares int_val's high half with s.str and would
             * truncate a 48-bit string pointer. */
            pn->desig_idx = (uint32_t)vla_dim_idx++;
            pn->left = (ASTRef)m->vla_dim; /* TU-arena dim expr (fallback) */
            if (m->base && (m->base->tag == TYPE_STRUCT || m->base->tag == TYPE_UNION) &&
                m->base->is_vla_struct) {
                /* VLA array of a VLA struct: the per-element size is the
                 * element's RUNTIME size (times the constant dims). */
                ASTRef sub = zc_vla_struct_size_ast(arena, dim_arena, m->base, m->base->struct_name);
                if (sub != AST_NULL) {
                    size_t extra = (m->base->size > 0) ? (m->vla_elem_size / m->base->size) : 1;
                    if (extra > 1)
                        sub = zc_vla_size_binop(arena, sub, zc_vla_size_const(arena, (int64_t)extra), TK_STAR);
                    piece = zc_vla_size_binop(arena, piece, sub, TK_STAR);
                }
            } else if (m->vla_elem_size > 1) {
                piece = zc_vla_size_binop(arena, piece, zc_vla_size_const(arena, (int64_t)m->vla_elem_size), TK_STAR);
            }
            tree = zc_vla_size_binop(arena, tree, piece, TK_PLUS);
            continue;
        }
        /* A member whose type is (an array of) a VLA struct contributes
         * its RUNTIME size per element. */
        const ZeroCCType *nested = NULL;
        size_t ncount = 1;
        if (m->tag == TYPE_STRUCT || m->tag == TYPE_UNION) {
            if (m->base) nested = m->base;
        } else if (m->tag == TYPE_ARRAY && m->base) {
            if (m->base->tag == TYPE_STRUCT || m->base->tag == TYPE_UNION) {
                nested = m->base;
                ncount = (m->base->size > 0) ? (m->size / m->base->size) : 0;
            }
        }
        if (nested && nested->is_vla_struct) {
            ASTRef sub = zc_vla_struct_size_ast(arena, dim_arena, nested, nested->struct_name);
            if (sub != AST_NULL) {
                if (!have_tree) {
                    tree = zc_vla_size_const(arena, cval);
                    have_tree = true;
                    cval = 0;
                }
                if (ncount > 1)
                    sub = zc_vla_size_binop(arena, sub, zc_vla_size_const(arena, (int64_t)ncount), TK_STAR);
                tree = zc_vla_size_binop(arena, tree, sub, TK_PLUS);
                continue;
            }
        }
        cval += (int64_t)m->size;
    }
    if (!have_tree) return zc_vla_size_const(arena, cval);
    return zc_vla_size_align_up(arena, tree, st->align);
}

#endif /* ZERO_CC_PARSER_VLA_STRUCT_H */
