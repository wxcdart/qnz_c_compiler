/* C23 compile-time function evaluation (CTFE) engine for constexpr functions:
 * local/parameter scope management, statement/control-flow evaluation
 * (blocks, if/else, while, for, break/continue/return, declarations), and
 * constexpr function call evaluation. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/parser/const_eval.h"

/* Result of evaluating a statement during CTFE. */
typedef enum {
    CTF_NORMAL,     /* statement completed normally */
    CTF_RETURN,     /* return <expr>; - result in .val */
    CTF_BREAK,      /* break */
    CTF_CONTINUE,   /* continue */
} CtfKind;

typedef struct {
    CtfKind kind;
    ZCConstVal val;
} CtfResult;

static CtfResult ctfe_eval_stmt(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_block(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_if(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_while(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_for(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_decl(ZeroCCCompiler *c, size_t *pos, size_t end);
static CtfResult ctfe_eval_expr_stmt(ZeroCCCompiler *c, size_t *pos, size_t end);

/* Push a new block scope marker. */
static void ctfe_push_scope(ZeroCCCompiler *c) {
    if (c->ctfe_scope_count >= c->ctfe_scope_cap) {
        size_t newcap = c->ctfe_scope_cap ? c->ctfe_scope_cap * 2 : 16;
        size_t *ns = (size_t *)realloc(c->ctfe_scope_stack, newcap * sizeof(size_t));
        if (!ns) return;
        c->ctfe_scope_stack = ns;
        c->ctfe_scope_cap = newcap;
    }
    c->ctfe_scope_stack[c->ctfe_scope_count++] = c->ctfe_local_count;
}

/* Pop the most recent block scope, removing locals declared inside it. */
static void ctfe_pop_scope(ZeroCCCompiler *c) {
    if (c->ctfe_scope_count == 0) return;
    size_t mark = c->ctfe_scope_stack[--c->ctfe_scope_count];
    while (c->ctfe_local_count > mark) {
        c->ctfe_local_count--;
        free((void *)c->ctfe_local_names[c->ctfe_local_count]);
        if (c->ctfe_local_bufs && c->ctfe_local_bufs[c->ctfe_local_count]) {
            free(c->ctfe_local_bufs[c->ctfe_local_count]);
            c->ctfe_local_bufs[c->ctfe_local_count] = NULL;
        }
    }
}

/* Declare a local variable (or reassign an existing one in the current scope). */
static void ctfe_declare_local_full(ZeroCCCompiler *c, const char *name,
                                    int64_t val_i, double val_r, uint8_t kind,
                                    uint32_t elem_size, uint8_t elem_tag,
                                    uint8_t *buf) {
    /* Check for existing declaration in the current (innermost) scope. */
    size_t mark = (c->ctfe_scope_count > 0) ? c->ctfe_scope_stack[c->ctfe_scope_count - 1] : c->ctfe_local_base;
    for (size_t idx = mark; idx < c->ctfe_local_count; idx++) {
        if (strcmp(c->ctfe_local_names[idx], name) == 0) {
            c->ctfe_local_values_i[idx] = val_i;
            c->ctfe_local_values_r[idx] = val_r;
            c->ctfe_local_kinds[idx] = kind;
            if (c->ctfe_local_elem_sizes) c->ctfe_local_elem_sizes[idx] = elem_size;
            if (c->ctfe_local_elem_tags) c->ctfe_local_elem_tags[idx] = elem_tag;
            if (c->ctfe_local_bufs) {
                if (c->ctfe_local_bufs[idx]) free(c->ctfe_local_bufs[idx]);
                c->ctfe_local_bufs[idx] = buf;
            }
            return;
        }
    }
    /* New declaration. */
    if (c->ctfe_local_count >= c->ctfe_local_cap) {
        size_t newcap = c->ctfe_local_cap ? c->ctfe_local_cap * 2 : 16;
        char **nn = (char **)realloc(c->ctfe_local_names, newcap * sizeof(char *));
        int64_t *nvi = (int64_t *)realloc(c->ctfe_local_values_i, newcap * sizeof(int64_t));
        double *nvr = (double *)realloc(c->ctfe_local_values_r, newcap * sizeof(double));
        uint8_t *nvk = (uint8_t *)realloc(c->ctfe_local_kinds, newcap * sizeof(uint8_t));
        uint32_t *nes = (uint32_t *)realloc(c->ctfe_local_elem_sizes, newcap * sizeof(uint32_t));
        uint8_t *net = (uint8_t *)realloc(c->ctfe_local_elem_tags, newcap * sizeof(uint8_t));
        uint8_t **nb = (uint8_t **)realloc(c->ctfe_local_bufs, newcap * sizeof(uint8_t *));
        if (!nn || !nvi || !nvr || !nvk || !nes || !net || !nb) {
            free(buf);
            return;
        }
        c->ctfe_local_names = nn;
        c->ctfe_local_values_i = nvi;
        c->ctfe_local_values_r = nvr;
        c->ctfe_local_kinds = nvk;
        c->ctfe_local_elem_sizes = nes;
        c->ctfe_local_elem_tags = net;
        c->ctfe_local_bufs = nb;
        c->ctfe_local_cap = newcap;
    }
    size_t idx = c->ctfe_local_count++;
    c->ctfe_local_names[idx] = strdup(name);
    c->ctfe_local_values_i[idx] = val_i;
    c->ctfe_local_values_r[idx] = val_r;
    c->ctfe_local_kinds[idx] = kind;
    c->ctfe_local_elem_sizes[idx] = elem_size;
    c->ctfe_local_elem_tags[idx] = elem_tag;
    c->ctfe_local_bufs[idx] = buf;
}

/* Evaluate a declaration inside a constexpr function body, supporting
 * scalar variables, pointers, local arrays (`T arr[N] = { ... }`,
 * `char s[] = "..."`), and comma-separated declarators. */
static CtfResult ctfe_eval_decl(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    while (*pos < end && (c->tokens[*pos].type == TK_CONSTEXPR ||
                          c->tokens[*pos].type == TK_CONST ||
                          c->tokens[*pos].type == TK_VOLATILE ||
                          c->tokens[*pos].type == TK_RESTRICT)) {
        (*pos)++;
    }
    bool is_auto = false;
    ZeroCCType base_ty = zero_cc_type_int();
    if (*pos < end && c->tokens[*pos].type == TK_AUTO) {
        is_auto = true;
        (*pos)++;
    } else if (*pos < end && zero_cc_tok_is_type_start(c, *pos)) {
        base_ty = zero_cc_parse_type_name_at_pos(c, pos);
        /* Walk back over any trailing '*' consumed by parse_type_name_at_pos
         * so each comma-separated declarator parses its own pointer stars. */
        while (base_ty.tag == TYPE_PTR && base_ty.base &&
               *pos > 0 && c->tokens[*pos - 1].type == TK_STAR) {
            base_ty = *base_ty.base;
            (*pos)--;
        }
    } else {
        while (*pos < end && c->tokens[*pos].type != TK_IDENT &&
               c->tokens[*pos].type != TK_STAR &&
               c->tokens[*pos].type != TK_SEMI) {
            (*pos)++;
        }
    }

    while (*pos < end && c->tokens[*pos].type != TK_SEMI) {
        ZeroCCType decl_ty = base_ty;
        while (*pos < end && (c->tokens[*pos].type == TK_STAR ||
                              c->tokens[*pos].type == TK_CONST ||
                              c->tokens[*pos].type == TK_VOLATILE ||
                              c->tokens[*pos].type == TK_RESTRICT)) {
            if (c->tokens[*pos].type == TK_STAR) {
                decl_ty = zero_cc_type_ptr(&decl_ty);
            }
            (*pos)++;
        }
        if (*pos >= end || c->tokens[*pos].type != TK_IDENT) {
            while (*pos < end && c->tokens[*pos].type != TK_SEMI) (*pos)++;
            break;
        }
        const char *name = c->tokens[*pos].str_val;
        (*pos)++;

        bool is_array = false;
        int64_t array_len = -1;
        if (*pos < end && c->tokens[*pos].type == TK_LBRACKET) {
            is_array = true;
            (*pos)++; /* consume '[' */
            if (*pos < end && c->tokens[*pos].type != TK_RBRACKET) {
                size_t saved = c->token_pos;
                c->token_pos = *pos;
                ZCConstVal dim_v = parse_conditional(c, &c->token_pos);
                *pos = c->token_pos;
                c->token_pos = saved;
                array_len = dim_v.i;
            }
            if (*pos < end && c->tokens[*pos].type == TK_RBRACKET) (*pos)++;
        }

        if (is_array) {
            uint32_t esz = (decl_ty.size > 0) ? (uint32_t)decl_ty.size : 4;
            uint8_t etag = (uint8_t)decl_ty.tag;
            uint8_t *buf = NULL;
            if (*pos < end && c->tokens[*pos].type == TK_ASSIGN) {
                (*pos)++; /* consume '=' */
                if (*pos < end && c->tokens[*pos].type == TK_STRING && esz == 1) {
                    size_t saved = c->token_pos;
                    c->token_pos = *pos;
                    ZCConstVal sv = parse_primary(c, &c->token_pos);
                    *pos = c->token_pos;
                    c->token_pos = saved;
                    const char *str = sv.ptr ? (const char *)sv.ptr : "";
                    size_t slen = strlen(str) + 1;
                    size_t total_elems = (array_len > 0) ? (size_t)array_len : slen;
                    buf = (uint8_t *)calloc(total_elems > 0 ? total_elems : 1, 1);
                    size_t copy_n = (slen < total_elems) ? slen : total_elems;
                    memcpy(buf, str, copy_n);
                } else if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                    (*pos)++; /* consume '{' */
                    ZCConstVal elems[256];
                    size_t ecnt = 0;
                    while (*pos < end && c->tokens[*pos].type != TK_RBRACE) {
                        size_t saved = c->token_pos;
                        c->token_pos = *pos;
                        ZCConstVal ev = parse_conditional(c, &c->token_pos);
                        *pos = c->token_pos;
                        c->token_pos = saved;
                        if (ecnt < 256) elems[ecnt++] = ev;
                        if (*pos < end && c->tokens[*pos].type == TK_COMMA) (*pos)++;
                    }
                    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++;
                    size_t total_elems = (array_len > 0 && (size_t)array_len > ecnt)
                                         ? (size_t)array_len : (ecnt > 0 ? ecnt : 1);
                    buf = (uint8_t *)calloc(total_elems, esz);
                    ZCConstVal p = zc_cv_ptr(buf, esz, etag);
                    for (size_t ei = 0; ei < ecnt; ei++) {
                        zc_cv_store(p, (int64_t)ei, elems[ei]);
                    }
                }
            }
            if (!buf) {
                size_t total_elems = (array_len > 0) ? (size_t)array_len : 1;
                buf = (uint8_t *)calloc(total_elems, esz);
            }
            ctfe_declare_local_full(c, name, (int64_t)(uintptr_t)buf, 0.0,
                                    (uint8_t)ZC_CV_PTR, esz, etag, buf);
        } else {
            ZCConstVal init_val = (decl_ty.tag == TYPE_PTR)
                                  ? zc_cv_ptr(NULL,
                                              (decl_ty.base && decl_ty.base->size > 0) ? (uint32_t)decl_ty.base->size : 1,
                                              (decl_ty.base) ? (uint8_t)decl_ty.base->tag : TYPE_INT8)
                                  : zc_cv_int(0);
            if (*pos < end && c->tokens[*pos].type == TK_ASSIGN) {
                (*pos)++; /* consume '=' */
                bool braced = (*pos < end && c->tokens[*pos].type == TK_LBRACE);
                if (braced) (*pos)++;
                if (!braced || (*pos < end && c->tokens[*pos].type != TK_RBRACE)) {
                    size_t saved = c->token_pos;
                    c->token_pos = *pos;
                    init_val = parse_conditional(c, &c->token_pos);
                    *pos = c->token_pos;
                    c->token_pos = saved;
                }
                if (braced) {
                    if (*pos < end && c->tokens[*pos].type == TK_COMMA) (*pos)++;
                    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++;
                }
                if (!is_auto) {
                    init_val = zc_cv_cast(init_val, decl_ty);
                }
            }
            uint32_t esz = (init_val.kind == ZC_CV_PTR)
                           ? init_val.ptr_elem_size
                           : ((decl_ty.size > 0) ? (uint32_t)decl_ty.size : 4);
            uint8_t etag = (init_val.kind == ZC_CV_PTR)
                           ? init_val.ptr_elem_tag
                           : (uint8_t)decl_ty.tag;
            ctfe_declare_local_full(c, name, init_val.i, init_val.r,
                                    (uint8_t)init_val.kind, esz, etag, NULL);
        }

        if (*pos < end && c->tokens[*pos].type == TK_COMMA) {
            (*pos)++;
            continue;
        }
        break;
    }

    if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
    }
    return r;
}

/* Evaluate an expression statement: `expr;`, supporting comma-separated expressions. */
static CtfResult ctfe_eval_expr_stmt(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);
    if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
        return r;
    }
    size_t saved = c->token_pos;
    c->token_pos = *pos;
    (void)parse_conditional(c, &c->token_pos);
    while (c->token_pos < end && c->tokens[c->token_pos].type == TK_COMMA) {
        c->token_pos++;
        (void)parse_conditional(c, &c->token_pos);
    }
    *pos = c->token_pos;
    c->token_pos = saved;
    if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
    return r;
}

/* Evaluate a block: `{ stmt* }`.  Assumes current token is '{'. */
static CtfResult ctfe_eval_block(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume '{' */
    ctfe_push_scope(c);

    while (*pos < end && c->tokens[*pos].type != TK_RBRACE) {
        size_t prev_pos = *pos;
        r = ctfe_eval_stmt(c, pos, end);
        if (r.kind != CTF_NORMAL) {
            int depth = 1;
            while (*pos < end && depth > 0) {
                if (c->tokens[*pos].type == TK_LBRACE) depth++;
                else if (c->tokens[*pos].type == TK_RBRACE) {
                    depth--;
                    if (depth == 0) break;
                }
                (*pos)++;
            }
            break;
        }
        if (*pos == prev_pos) (*pos)++;
    }

    ctfe_pop_scope(c);

    if (*pos < end && c->tokens[*pos].type == TK_RBRACE) (*pos)++; /* consume '}' */
    return r;
}

/* Evaluate an if/else statement. */
static CtfResult ctfe_eval_if(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'if' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;
    size_t saved = c->token_pos;
    c->token_pos = *pos;
    ZCConstVal cond = parse_conditional(c, &c->token_pos);
    *pos = c->token_pos;
    c->token_pos = saved;
    if (*pos < end && c->tokens[*pos].type == TK_RPAREN) (*pos)++;

    if (zc_cv_is_true(cond)) {
        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            r = ctfe_eval_block(c, pos, end);
        } else {
            r = ctfe_eval_stmt(c, pos, end);
        }
        if (r.kind == CTF_NORMAL && *pos < end && c->tokens[*pos].type == TK_ELSE) {
            (*pos)++; /* consume 'else' */
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                int depth = 1;
                (*pos)++;
                while (*pos < end && depth > 0) {
                    if (c->tokens[*pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                    (*pos)++;
                }
            } else {
                while (*pos < end &&
                       c->tokens[*pos].type != TK_SEMI &&
                       c->tokens[*pos].type != TK_RBRACE) {
                    if (c->tokens[*pos].type == TK_LPAREN ||
                        c->tokens[*pos].type == TK_LBRACE) {
                        int depth = 1;
                        (*pos)++;
                        while (*pos < end && depth > 0) {
                            if (c->tokens[*pos].type == TK_LPAREN ||
                                c->tokens[*pos].type == TK_LBRACE) depth++;
                            else if (c->tokens[*pos].type == TK_RPAREN ||
                                     c->tokens[*pos].type == TK_RBRACE) depth--;
                            (*pos)++;
                        }
                    } else {
                        (*pos)++;
                    }
                }
                if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
            }
        }
    } else {
        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            int depth = 1;
            (*pos)++;
            while (*pos < end && depth > 0) {
                if (c->tokens[*pos].type == TK_LBRACE) depth++;
                else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                (*pos)++;
            }
        } else {
            while (*pos < end &&
                   c->tokens[*pos].type != TK_SEMI &&
                   c->tokens[*pos].type != TK_RBRACE &&
                   c->tokens[*pos].type != TK_ELSE) {
                if (c->tokens[*pos].type == TK_LPAREN ||
                    c->tokens[*pos].type == TK_LBRACE) {
                    int depth = 1;
                    (*pos)++;
                    while (*pos < end && depth > 0) {
                        if (c->tokens[*pos].type == TK_LPAREN ||
                            c->tokens[*pos].type == TK_LBRACE) depth++;
                        else if (c->tokens[*pos].type == TK_RPAREN ||
                                 c->tokens[*pos].type == TK_RBRACE) depth--;
                        (*pos)++;
                    }
                } else {
                    (*pos)++;
                }
            }
            if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        }
        if (*pos < end && c->tokens[*pos].type == TK_ELSE) {
            (*pos)++; /* consume 'else' */
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                r = ctfe_eval_block(c, pos, end);
            } else {
                r = ctfe_eval_stmt(c, pos, end);
            }
        }
    }
    return r;
}

/* Evaluate a while loop. */
static CtfResult ctfe_eval_while(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'while' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;

    int iterations = 0;
    const int max_iter = 100000;

    while (*pos < end) {
        size_t cond_start = *pos;
        size_t saved = c->token_pos;
        c->token_pos = *pos;
        ZCConstVal cond = parse_conditional(c, &c->token_pos);
        *pos = c->token_pos;
        c->token_pos = saved;
        if (*pos < end && c->tokens[*pos].type == TK_RPAREN) (*pos)++;

        if (!zc_cv_is_true(cond)) {
            if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
                int depth = 1;
                (*pos)++;
                while (*pos < end && depth > 0) {
                    if (c->tokens[*pos].type == TK_LBRACE) depth++;
                    else if (c->tokens[*pos].type == TK_RBRACE) depth--;
                    (*pos)++;
                }
            } else {
                while (*pos < end &&
                       c->tokens[*pos].type != TK_SEMI &&
                       c->tokens[*pos].type != TK_RBRACE) {
                    if (c->tokens[*pos].type == TK_LPAREN ||
                        c->tokens[*pos].type == TK_LBRACE) {
                        int depth = 1;
                        (*pos)++;
                        while (*pos < end && depth > 0) {
                            if (c->tokens[*pos].type == TK_LPAREN ||
                                c->tokens[*pos].type == TK_LBRACE) depth++;
                            else if (c->tokens[*pos].type == TK_RPAREN ||
                                     c->tokens[*pos].type == TK_RBRACE) depth--;
                            (*pos)++;
                        }
                    } else {
                        (*pos)++;
                    }
                }
                if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
            }
            break;
        }

        if (*pos < end && c->tokens[*pos].type == TK_LBRACE) {
            r = ctfe_eval_block(c, pos, end);
        } else {
            r = ctfe_eval_stmt(c, pos, end);
        }
        if (r.kind == CTF_BREAK) { r.kind = CTF_NORMAL; break; }
        if (r.kind == CTF_RETURN) break;
        if (r.kind == CTF_CONTINUE) r.kind = CTF_NORMAL;

        *pos = cond_start;
        if (++iterations > max_iter) {
            r.kind = CTF_NORMAL;
            break;
        }
    }
    return r;
}

/* Helper to advance past a for-loop body starting at `bstart`. */
static size_t ctfe_skip_for_body(ZeroCCCompiler *c, size_t bstart, size_t end) {
    if (bstart < end && c->tokens[bstart].type == TK_LBRACE) {
        int depth2 = 1;
        bstart++;
        while (bstart < end && depth2 > 0) {
            if (c->tokens[bstart].type == TK_LBRACE) depth2++;
            else if (c->tokens[bstart].type == TK_RBRACE) depth2--;
            bstart++;
        }
    } else {
        while (bstart < end &&
               c->tokens[bstart].type != TK_SEMI &&
               c->tokens[bstart].type != TK_RBRACE) {
            if (c->tokens[bstart].type == TK_LPAREN ||
                c->tokens[bstart].type == TK_LBRACE) {
                int depth2 = 1;
                bstart++;
                while (bstart < end && depth2 > 0) {
                    if (c->tokens[bstart].type == TK_LPAREN ||
                        c->tokens[bstart].type == TK_LBRACE) depth2++;
                    else if (c->tokens[bstart].type == TK_RPAREN ||
                             c->tokens[bstart].type == TK_RBRACE) depth2--;
                    bstart++;
                }
            } else {
                bstart++;
            }
        }
        if (bstart < end && c->tokens[bstart].type == TK_SEMI) bstart++;
    }
    return bstart;
}

/* Evaluate a for loop: `for (init; cond; incr) body`. */
static CtfResult ctfe_eval_for(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    (*pos)++; /* consume 'for' */
    if (*pos < end && c->tokens[*pos].type == TK_LPAREN) (*pos)++;

    ctfe_push_scope(c);

    /* Init (a declaration or expression). */
    if (*pos < end && c->tokens[*pos].type != TK_SEMI) {
        ZeroCCTokType it = c->tokens[*pos].type;
        if (zero_cc_tok_is_type_start(c, *pos) || it == TK_AUTO || it == TK_CONSTEXPR) {
            r = ctfe_eval_decl(c, pos, end);
        } else {
            r = ctfe_eval_expr_stmt(c, pos, end);
        }
        if (r.kind != CTF_NORMAL) {
            ctfe_pop_scope(c);
            return r;
        }
    } else if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
        (*pos)++;
    }

    size_t cond_start = *pos;

    size_t incr_start = cond_start;
    int depth = 0;
    for (size_t scan = cond_start; scan < end; scan++) {
        ZeroCCTokType tt = c->tokens[scan].type;
        if (tt == TK_LPAREN) depth++;
        else if (tt == TK_RPAREN) { if (depth > 0) depth--; }
        else if (tt == TK_SEMI && depth == 0) {
            incr_start = scan + 1;
            break;
        }
    }

    size_t body_pos = cond_start;
    depth = 0;
    while (body_pos < end) {
        ZeroCCTokType tt = c->tokens[body_pos].type;
        if (tt == TK_LPAREN) depth++;
        else if (tt == TK_RPAREN) {
            if (depth == 0) break;
            depth--;
        }
        body_pos++;
    }
    size_t bstart = body_pos + 1;

    int iterations = 0;
    const int max_iter = 100000;

    for (;;) {
        if (cond_start < end && c->tokens[cond_start].type == TK_SEMI) {
            /* Empty condition = true. */
        } else {
            size_t saved = c->token_pos;
            c->token_pos = cond_start;
            ZCConstVal cond = parse_conditional(c, &c->token_pos);
            c->token_pos = saved;
            if (!zc_cv_is_true(cond)) {
                *pos = ctfe_skip_for_body(c, bstart, end);
                break;
            }
        }

        size_t bp = bstart;
        if (bstart < end && c->tokens[bstart].type == TK_LBRACE) {
            r = ctfe_eval_block(c, &bp, end);
        } else {
            r = ctfe_eval_stmt(c, &bp, end);
        }
        if (r.kind == CTF_BREAK) {
            r.kind = CTF_NORMAL;
            *pos = ctfe_skip_for_body(c, bstart, end);
            break;
        }
        if (r.kind == CTF_RETURN) break;
        if (r.kind == CTF_CONTINUE) r.kind = CTF_NORMAL;

        if (incr_start < end && incr_start < body_pos &&
            c->tokens[incr_start].type != TK_RPAREN) {
            size_t saved = c->token_pos;
            c->token_pos = incr_start;
            (void)parse_conditional(c, &c->token_pos);
            while (c->token_pos < body_pos && c->tokens[c->token_pos].type == TK_COMMA) {
                c->token_pos++;
                (void)parse_conditional(c, &c->token_pos);
            }
            c->token_pos = saved;
        }

        if (++iterations > max_iter) {
            r.kind = CTF_NORMAL;
            *pos = ctfe_skip_for_body(c, bstart, end);
            break;
        }
    }

    ctfe_pop_scope(c);
    return r;
}

/* Evaluate a single statement during CTFE. */
static CtfResult ctfe_eval_stmt(ZeroCCCompiler *c, size_t *pos, size_t end) {
    CtfResult r;
    r.kind = CTF_NORMAL;
    r.val = zc_cv_int(0);

    if (*pos >= end) return r;

    ZeroCCTokType t = c->tokens[*pos].type;

    if (t == TK_LBRACE) {
        return ctfe_eval_block(c, pos, end);
    }
    if (t == TK_IF) {
        return ctfe_eval_if(c, pos, end);
    }
    if (t == TK_WHILE) {
        return ctfe_eval_while(c, pos, end);
    }
    if (t == TK_FOR) {
        return ctfe_eval_for(c, pos, end);
    }
    if (t == TK_RETURN) {
        (*pos)++; /* consume 'return' */
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) {
            (*pos)++;
            r.kind = CTF_RETURN;
            return r;
        }
        size_t saved = c->token_pos;
        c->token_pos = *pos;
        r.val = parse_conditional(c, &c->token_pos);
        *pos = c->token_pos;
        c->token_pos = saved;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_RETURN;
        return r;
    }
    if (t == TK_RPAREN) {
        (*pos)++;
        r.kind = CTF_NORMAL;
        return r;
    }
    if (t == TK_BREAK) {
        (*pos)++;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_BREAK;
        return r;
    }
    if (t == TK_CONTINUE) {
        (*pos)++;
        if (*pos < end && c->tokens[*pos].type == TK_SEMI) (*pos)++;
        r.kind = CTF_CONTINUE;
        return r;
    }
    if (zero_cc_tok_is_type_start(c, *pos) || t == TK_AUTO || t == TK_CONSTEXPR) {
        return ctfe_eval_decl(c, pos, end);
    }

    return ctfe_eval_expr_stmt(c, pos, end);
}

/* Evaluate a constexpr function body between [start, end).  Returns the
 * value of the return statement, or 0 if no return was reached. */
ZCConstVal ctfe_eval_body(ZeroCCCompiler *c, size_t start, size_t end) {
    size_t pos = start;
    if (pos < end && c->tokens[pos].type == TK_LBRACE) pos++;

    ctfe_push_scope(c);
    ZCConstVal result = zc_cv_int(0);
    while (pos < end) {
        if (c->tokens[pos].type == TK_RBRACE) {
            pos++;
            break;
        }
        size_t prev_pos = pos;
        CtfResult r = ctfe_eval_stmt(c, &pos, end);
        if (r.kind == CTF_RETURN) {
            result = r.val;
            break;
        }
        if (pos == prev_pos) pos++;
    }
    ctfe_pop_scope(c);
    return result;
}

/* Evaluate a C23 constexpr function call `f(args)` at token position *pos. */
ZCConstVal ctfe_eval_call(ZeroCCCompiler *c, ZeroCCFunction *fn, size_t *pos) {
    size_t call_pos = *pos;
    call_pos++; /* identifier */
    call_pos++; /* '(' */

    ZCConstVal arg_values[16];
    size_t arg_count = 0;
    while (call_pos < c->token_count &&
           c->tokens[call_pos].type != TK_RPAREN &&
           arg_count < 16) {
        size_t saved_pos = c->token_pos;
        c->token_pos = call_pos;
        ZCConstVal arg_val = parse_conditional(c, &c->token_pos);
        call_pos = c->token_pos;
        c->token_pos = saved_pos;
        arg_values[arg_count++] = arg_val;
        if (call_pos < c->token_count &&
            c->tokens[call_pos].type == TK_COMMA) {
            call_pos++; /* consume ',' */
        }
    }
    if (call_pos < c->token_count &&
        c->tokens[call_pos].type == TK_RPAREN) {
        call_pos++;
    }

    const char **saved_param_names = c->ctfe_param_names;
    int64_t saved_param_values_i[16];
    double saved_param_values_r[16];
    uint8_t saved_param_kinds[16];
    uint32_t saved_param_elem_sizes[16];
    uint8_t saved_param_elem_tags[16];
    size_t saved_param_count = c->ctfe_param_count;
    size_t saved_local_base = c->ctfe_local_base;

    for (size_t ai = 0; ai < c->ctfe_param_count; ai++) {
        saved_param_values_i[ai] = c->ctfe_param_values_i[ai];
        saved_param_values_r[ai] = c->ctfe_param_values_r[ai];
        saved_param_kinds[ai] = c->ctfe_param_kinds[ai];
        saved_param_elem_sizes[ai] = c->ctfe_param_elem_sizes[ai];
        saved_param_elem_tags[ai] = c->ctfe_param_elem_tags[ai];
    }

    c->ctfe_param_names = (const char **)fn->param_names;
    for (size_t ai = 0; ai < arg_count; ai++) {
        ZCConstVal av = arg_values[ai];
        if (fn->param_types && ai < fn->param_count) {
            av = zc_cv_cast(av, fn->param_types[ai]);
        }
        c->ctfe_param_values_i[ai] = av.i;
        c->ctfe_param_values_r[ai] = av.r;
        c->ctfe_param_kinds[ai] = (uint8_t)av.kind;
        c->ctfe_param_elem_sizes[ai] = av.ptr_elem_size;
        c->ctfe_param_elem_tags[ai] = av.ptr_elem_tag;
    }
    c->ctfe_param_count = (fn->param_count < 16) ? fn->param_count : 16;
    c->ctfe_local_base = c->ctfe_local_count;

    size_t saved_token_pos = c->token_pos;
    c->token_pos = fn->constexpr_body_start;

    ZCConstVal result = ctfe_eval_body(c, fn->constexpr_body_start,
                                       fn->constexpr_body_end);
    result = zc_cv_cast(result, fn->return_type);

    c->ctfe_param_names = saved_param_names;
    for (size_t ai = 0; ai < saved_param_count; ai++) {
        c->ctfe_param_values_i[ai] = saved_param_values_i[ai];
        c->ctfe_param_values_r[ai] = saved_param_values_r[ai];
        c->ctfe_param_kinds[ai] = saved_param_kinds[ai];
        c->ctfe_param_elem_sizes[ai] = saved_param_elem_sizes[ai];
        c->ctfe_param_elem_tags[ai] = saved_param_elem_tags[ai];
    }
    c->ctfe_param_count = saved_param_count;
    c->ctfe_local_base = saved_local_base;
    c->token_pos = saved_token_pos;

    *pos = call_pos;
    return result;
}
