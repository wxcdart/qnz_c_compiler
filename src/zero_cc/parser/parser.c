/* =========================================================================
 *  parser.c
 *  C99 AST Parser and Code Generator
 *
 *  This file acts as a wrapper that includes the split parser modules.
 *  ========================================================================= */

#include "zero_cc/types/types.h"
#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/ast_arena.h"
#include "zero_cc/parser/ast_expr.h"
#include "zero_cc/parser/ast_stmt.h"
#include "zero_cc/parser/ast_dump.h"

/* Public entry point for Expression AST parsing (definition moved here;
 * ast_expr.h is included by many modules, so the body must live in a
 * single TU). */
ASTRef zero_cc_parse_expr_ast(ZeroCCCompiler *c, ZeroCCASTArena *arena) {
    if (!c || !arena) return AST_NULL;
    return parse_expr_ast_internal(c, arena);
}

/* AST arena lifecycle & body-audit walkers (moved from ast_arena.h:
 * single-definition TU - the header is now declarations-only). */
void zero_cc_ast_arena_init(ZeroCCASTArena *arena, size_t initial_cap) {
    if (!arena) return;
    (void)initial_cap;
    arena->chunk_capacity = 8;
    arena->chunks = (ZeroCCASTNode **)calloc(arena->chunk_capacity, sizeof(ZeroCCASTNode *));
    if (!arena->chunks) {
        arena->chunk_count = 0;
        arena->count = 0;
        return;
    }
    arena->chunks[0] = (ZeroCCASTNode *)calloc(ZERO_CC_AST_CHUNK_SIZE, sizeof(ZeroCCASTNode));
    if (!arena->chunks[0]) {
        free(arena->chunks);
        arena->chunks = NULL;
        arena->chunk_capacity = 0;
        arena->chunk_count = 0;
        arena->count = 0;
        return;
    }
    arena->chunk_count = 1;
    arena->count = 1; /* Reserve index 0 for AST_NULL */
}

void zero_cc_ast_arena_free(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->chunks) {
        for (size_t i = 0; i < arena->chunk_count; i++) {
            if (arena->chunks[i]) free(arena->chunks[i]);
        }
        free(arena->chunks);
        arena->chunks = NULL;
    }
    arena->chunk_count = 0;
    arena->chunk_capacity = 0;
    arena->count = 0;
}

void zero_cc_ast_arena_reset(ZeroCCASTArena *arena) {
    if (!arena) return;
    if (arena->chunk_count > 0 && arena->chunks && arena->chunks[0]) {
        memset(&arena->chunks[0][0], 0, sizeof(ZeroCCASTNode));
        arena->count = 1; /* Keep index 0 as AST_NULL, reuse allocated chunks */
    } else {
        arena->count = 0;
    }
}

ASTRef zero_cc_ast_alloc_node(ZeroCCASTArena *arena, ZeroCCASTTag tag) {
    if (!arena || !arena->chunks) return AST_NULL;

    size_t chunk_idx = arena->count >> ZERO_CC_AST_CHUNK_BITS;
    size_t node_idx  = arena->count & ZERO_CC_AST_CHUNK_MASK;

    if (chunk_idx >= arena->chunk_count) {
        if (chunk_idx >= arena->chunk_capacity) {
            size_t new_cap = arena->chunk_capacity ? arena->chunk_capacity * 2 : 8;
            ZeroCCASTNode **new_chunks = (ZeroCCASTNode **)realloc(arena->chunks, new_cap * sizeof(ZeroCCASTNode *));
            if (!new_chunks) return AST_NULL;
            arena->chunks = new_chunks;
            arena->chunk_capacity = new_cap;
        }
        ZeroCCASTNode *new_chunk = (ZeroCCASTNode *)calloc(ZERO_CC_AST_CHUNK_SIZE, sizeof(ZeroCCASTNode));
        if (!new_chunk) return AST_NULL;
        arena->chunks[chunk_idx] = new_chunk;
        arena->chunk_count++;
    }

    ASTRef ref = (ASTRef)arena->count++;
    ZeroCCASTNode *node = &arena->chunks[chunk_idx][node_idx];
    memset(node, 0, sizeof(*node));
    node->tag = (uint16_t)tag;
    return ref;
}

static inline size_t zero_cc_ast_node_count_impl(ZeroCCASTArena *arena, ASTRef ref, int depth) {
    if (!arena || ref == AST_NULL || depth > 64) return 0;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return 0;

    size_t cnt = 1;
    if (n->left != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->left, depth + 1);
    if (n->right != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->right, depth + 1);
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.alt, depth + 1);
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.step, depth + 1);
        if (n->aux.alt != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->aux.alt, depth + 1);
    }
    if (n->next != AST_NULL) cnt += zero_cc_ast_node_count_impl(arena, n->next, depth + 1);
    return cnt;
}

size_t zero_cc_ast_node_count(ZeroCCASTArena *arena, ASTRef root) {
    return zero_cc_ast_node_count_impl(arena, root, 0);
}

static inline ASTRef zero_cc_ast_clone_impl(ZeroCCASTArena *src_arena, ASTRef src_ref, ZeroCCASTArena *dst_arena, int depth) {
    if (!src_arena || src_ref == AST_NULL || !dst_arena || depth > 64) return AST_NULL;
    ZeroCCASTNode *src_n = zero_cc_ast_get(src_arena, src_ref);
    if (!src_n) return AST_NULL;

    ASTRef dst_ref = zero_cc_ast_alloc_node(dst_arena, (ZeroCCASTTag)src_n->tag);
    if (dst_ref == AST_NULL) return AST_NULL;
    ZeroCCASTNode *dst_n = zero_cc_ast_get(dst_arena, dst_ref);
    if (!dst_n) return AST_NULL;

    /* Copy entire node structure */
    *dst_n = *src_n;

    /* Recursively clone children */
    if (src_n->left != AST_NULL) {
        dst_n->left = zero_cc_ast_clone_impl(src_arena, src_n->left, dst_arena, depth + 1);
    }
    if (src_n->right != AST_NULL) {
        dst_n->right = zero_cc_ast_clone_impl(src_arena, src_n->right, dst_arena, depth + 1);
    }
    if (src_n->tag == NODE_IF || src_n->tag == NODE_TERNARY) {
        if (src_n->aux.alt != AST_NULL) {
            dst_n->aux.alt = zero_cc_ast_clone_impl(src_arena, src_n->aux.alt, dst_arena, depth + 1);
        }
    } else if (src_n->tag == NODE_FOR) {
        if (src_n->aux.step != AST_NULL) {
            dst_n->aux.step = zero_cc_ast_clone_impl(src_arena, src_n->aux.step, dst_arena, depth + 1);
        }
        if (src_n->aux.alt != AST_NULL) {
            dst_n->aux.alt = zero_cc_ast_clone_impl(src_arena, src_n->aux.alt, dst_arena, depth + 1);
        }
    }
    if (src_n->next != AST_NULL) {
        dst_n->next = zero_cc_ast_clone_impl(src_arena, src_n->next, dst_arena, depth + 1);
    }

    return dst_ref;
}

ASTRef zero_cc_ast_clone(ZeroCCASTArena *src_arena, ASTRef root, ZeroCCASTArena *dst_arena) {
    return zero_cc_ast_clone_impl(src_arena, root, dst_arena, 0);
}

static inline bool zero_cc_ast_has_call_impl(ZeroCCASTArena *arena, ASTRef ref, const char *target_name, int depth) {
    if (!arena || ref == AST_NULL || depth > 64) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;

    if (n->tag == NODE_CALL) {
        if (!target_name) return true;
        ZeroCCASTNode *callee = zero_cc_ast_get(arena, n->left);
        if (callee && callee->tag == NODE_IDENT && callee->s.str) {
            if (strcmp(callee->s.str, target_name) == 0) return true;
        }
    }

    if (n->left != AST_NULL && zero_cc_ast_has_call_impl(arena, n->left, target_name, depth + 1)) return true;
    if (n->right != AST_NULL && zero_cc_ast_has_call_impl(arena, n->right, target_name, depth + 1)) return true;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.alt, target_name, depth + 1)) return true;
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.step, target_name, depth + 1)) return true;
        if (n->aux.alt != AST_NULL && zero_cc_ast_has_call_impl(arena, n->aux.alt, target_name, depth + 1)) return true;
    }
    if (n->next != AST_NULL && zero_cc_ast_has_call_impl(arena, n->next, target_name, depth + 1)) return true;
    return false;
}

bool zero_cc_ast_has_call(ZeroCCASTArena *arena, ASTRef root, const char *target_name) {
    return zero_cc_ast_has_call_impl(arena, root, target_name, 0);
}

/*
 * Sprint 35 Phase 1 (inline audit): body features the call-site inliner
 * cannot lower safely.  Each predicate walks left/right/next plus the
 * if/ternary else (aux.alt) and for-loop step/body (aux.step/aux.alt),
 * mirroring zero_cc_ast_has_call_impl.
 */

static inline bool zero_cc_ast_walk(ZeroCCASTArena *arena, ASTRef ref, int depth,
                                    bool (*pred)(ZeroCCASTNode *, size_t), size_t ctx) {
    (void)depth;
    if (!arena || ref == AST_NULL || depth > 128) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    if (pred(n, ctx)) return true;
    if (n->left != AST_NULL && zero_cc_ast_walk(arena, n->left, depth + 1, pred, ctx)) return true;
    if (n->right != AST_NULL && zero_cc_ast_walk(arena, n->right, depth + 1, pred, ctx)) return true;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL && zero_cc_ast_walk(arena, n->aux.alt, depth + 1, pred, ctx)) return true;
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL && zero_cc_ast_walk(arena, n->aux.step, depth + 1, pred, ctx)) return true;
        if (n->aux.alt != AST_NULL && zero_cc_ast_walk(arena, n->aux.alt, depth + 1, pred, ctx)) return true;
    }
    if (n->next != AST_NULL && zero_cc_ast_walk(arena, n->next, depth + 1, pred, ctx)) return true;
    return false;
}

/* True if the body declares ANY local variable (including for-loop init
 * declarations and static locals).  Local symbols are registered at parse
 * time into the callee's per-function table, which is not visible from the
 * caller where the body is re-lowered: unregistered locals lower to
 * TEMP_NONE and silently corrupt the call site. */
static bool zero_cc_ast_pred_local_decl(ZeroCCASTNode *n, size_t ctx) {
    (void)ctx;
    return n->tag == NODE_DECL && !(n->flags & NODE_DECL_FUNC_PROTO);
}
bool zero_cc_ast_has_local_decl(ZeroCCASTArena *arena, ASTRef root) {
    return zero_cc_ast_walk(arena, root, 0, zero_cc_ast_pred_local_decl, 0);
}

/* True if the body contains a label or goto.  User labels resolve through
 * a global name-based map (get_or_create_user_ir_label), so a label in an
 * inlined body with the same name as a caller label would alias the
 * caller's target. */
static bool zero_cc_ast_pred_label_goto(ZeroCCASTNode *n, size_t ctx) {
    (void)ctx;
    return n->tag == NODE_LABEL || n->tag == NODE_GOTO;
}
bool zero_cc_ast_has_label_goto(ZeroCCASTArena *arena, ASTRef root) {
    return zero_cc_ast_walk(arena, root, 0, zero_cc_ast_pred_label_goto, 0);
}

/* True if the body takes the address of a value parameter (&param).
 * Inlined parameters are bound to virtual registers with no stack slot,
 * so &param lowers to an invalid LEA (the Sprint 35 Phase 1 audit found
 * this segfaulting at -O2). */
static bool zero_cc_ast_addr_of_param_impl(ZeroCCASTArena *arena, ASTRef ref, int depth,
                                           const char *param_names[], size_t param_count) {
    if (!arena || ref == AST_NULL || depth > 128) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    if (n->tag == NODE_UNOP && n->aux.op == TK_AMP && n->left != AST_NULL) {
        ZeroCCASTNode *op = zero_cc_ast_get(arena, n->left);
        if (op && op->tag == NODE_IDENT && op->s.str) {
            for (size_t p = 0; p < param_count; p++) {
                if (param_names[p] && strcmp(op->s.str, param_names[p]) == 0) return true;
            }
        }
    }
    if (n->left != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->left, depth + 1, param_names, param_count)) return true;
    if (n->right != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->right, depth + 1, param_names, param_count)) return true;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->aux.alt, depth + 1, param_names, param_count)) return true;
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->aux.step, depth + 1, param_names, param_count)) return true;
        if (n->aux.alt != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->aux.alt, depth + 1, param_names, param_count)) return true;
    }
    if (n->next != AST_NULL && zero_cc_ast_addr_of_param_impl(arena, n->next, depth + 1, param_names, param_count)) return true;
    return false;
}
bool zero_cc_ast_addr_of_param(ZeroCCASTArena *arena, ASTRef root,
                               const char *param_names[], int param_count) {
    if (param_count <= 0) return false;
    return zero_cc_ast_addr_of_param_impl(arena, root, 0, param_names, (size_t)param_count);
}

/* True if the body assigns to a value parameter (x = ... / x op= ...).
 * Inlined parameters bind to the caller's argument virtual registers; a write
 * to a parameter has no matching stack slot, so the store is dropped while a
 * later read of the parameter still sees the original argument -- e.g.
 * `int bump(int x){ x += 100; return x*2; }` inlined as `v*2` (the `+100` vanishes,
 * Sprint 41). Such functions must not be inlined. (Increment/decrement are not
 * supported, so NODE_ASSIGN covers every write form. */
static bool zero_cc_ast_assigns_to_param_impl(ZeroCCASTArena *arena, ASTRef ref, int depth,
                                              const char *param_names[], size_t param_count) {
    if (!arena || ref == AST_NULL || depth > 128) return false;
    ZeroCCASTNode *n = zero_cc_ast_get(arena, ref);
    if (!n) return false;
    if ((n->tag == NODE_ASSIGN || (n->tag == NODE_UNOP && (n->aux.op == TK_INC || n->aux.op == TK_DEC))) && n->left != AST_NULL) {
        ZeroCCASTNode *lhs = zero_cc_ast_get(arena, n->left);
        if (lhs && lhs->tag == NODE_IDENT && lhs->s.str) {
            for (size_t p = 0; p < param_count; p++) {
                if (param_names[p] && strcmp(lhs->s.str, param_names[p]) == 0) return true;
            }
        }
    }
    if (n->left != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->left, depth + 1, param_names, param_count)) return true;
    if (n->right != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->right, depth + 1, param_names, param_count)) return true;
    if (n->tag == NODE_IF || n->tag == NODE_TERNARY) {
        if (n->aux.alt != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->aux.alt, depth + 1, param_names, param_count)) return true;
    } else if (n->tag == NODE_FOR) {
        if (n->aux.step != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->aux.step, depth + 1, param_names, param_count)) return true;
        if (n->aux.alt != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->aux.alt, depth + 1, param_names, param_count)) return true;
    }
    if (n->next != AST_NULL && zero_cc_ast_assigns_to_param_impl(arena, n->next, depth + 1, param_names, param_count)) return true;
    return false;
}
bool zero_cc_ast_assigns_to_param(ZeroCCASTArena *arena, ASTRef root,
                                                  const char *param_names[], int param_count) {
    if (param_count <= 0) return false;
    return zero_cc_ast_assigns_to_param_impl(arena, root, 0, param_names, (size_t)param_count);
}


static __thread ZeroCCTypeNode *g_type_pool_head = NULL;

void zero_cc_type_pool_free(void) {
    ZeroCCTypeNode *curr = g_type_pool_head;
    while (curr) {
        ZeroCCTypeNode *next = curr->next;
        free(curr);
        curr = next;
    }
    g_type_pool_head = NULL;
}

/* Owner-set helpers for zero_cc_free_all_struct_arrays: collect distinct
 * member-array pointers; an array referenced by several types (the
 * canonical struct def + typedef aliases + locals/params of that type)
 * is freed exactly once. */
#define ZC_STRUCT_OWNER_SET_CAP 1024 /* > any sane distinct-struct count */

static void zc_struct_owner_add(ZeroCCStructMember **set, size_t *n,
                                ZeroCCStructMember *m) {
    if (!m) return;
    for (size_t i = 0; i < *n; i++)
        if (set[i] == m) return;
    if (*n < ZC_STRUCT_OWNER_SET_CAP) set[(*n)++] = m;
    /* Set full: skip — leaking one array beats risking a double free. */
}

void zero_cc_free_all_struct_arrays(ZeroCCCompiler *c) {
    ZeroCCStructMember *owners[ZC_STRUCT_OWNER_SET_CAP];
    size_t n = 0;
    if (c) {
        for (size_t i = 0; i < c->struct_count; i++)
            zc_struct_owner_add(owners, &n, c->structs[i].type.members);
        for (size_t i = 0; i < c->typedef_count; i++)
            zc_struct_owner_add(owners, &n, c->typedefs[i].type.members);
        for (size_t i = 0; i < c->local_count; i++)
            zc_struct_owner_add(owners, &n, c->locals[i].type.members);
        for (size_t i = 0; i < c->function_count; i++) {
            zc_struct_owner_add(owners, &n, c->functions[i].return_type.members);
            for (size_t j = 0; j < c->functions[i].param_count && j < 8; j++)
                zc_struct_owner_add(owners, &n,
                                    c->functions[i].param_types[j].members);
        }
        for (size_t i = 0; i < c->thread_local_count && i < 64; i++)
            zc_struct_owner_add(owners, &n, c->thread_locals[i].type.members);
    }
    /* The pool nodes are the live holders of nested anonymous-struct
     * member arrays — iterate the list directly (chasing `base` pointers
     * from the tables would dangle once the pool is freed). */
    for (ZeroCCTypeNode *node = g_type_pool_head; node; node = node->next)
        zc_struct_owner_add(owners, &n, node->type.members);
    for (size_t i = 0; i < n; i++) free(owners[i]);
    /* The structs/typedefs tables persist past the compile: null the
     * now-dangling owners so a stale entry can't be freed twice. */
    if (c) {
        for (size_t i = 0; i < c->struct_count; i++) {
            c->structs[i].type.members = NULL;
            c->structs[i].type.members_cap = 0;
        }
        for (size_t i = 0; i < c->typedef_count; i++) {
            c->typedefs[i].type.members = NULL;
            c->typedefs[i].type.members_cap = 0;
        }
    }
}

ZeroCCType *zero_cc_type_alloc_base(const ZeroCCType *base) {
    if (!base) return NULL;
    ZeroCCTypeNode *node = (ZeroCCTypeNode *)malloc(sizeof(ZeroCCTypeNode));
    if (!node) return NULL;
    node->type = *base;
    node->next = g_type_pool_head;
    g_type_pool_head = node;
    return &node->type;
}

/* Build-integrity canary: lets the driver detect stale object files compiled
 * against an older ZeroCCASTNode layout. A struct-size change without a full
 * rebuild silently corrupts AST thin pointers (designated-init string
 * pointers, int64 constants) and crashes far from the real cause. */
size_t zero_cc_ast_node_size(void) { return sizeof(ZeroCCASTNode); }
