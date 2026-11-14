/* Compile-time constant evaluation (Sprint 114 Tier C: implementation moved
 * to const_eval.c). This header keeps the ZCConstVal value model and the
 * entry-point prototypes; every consumer TU links the single copy in
 * const_eval.o instead of compiling its own static-inline copies. */
#ifndef ZERO_CC_PARSER_CONST_EVAL_H
#define ZERO_CC_PARSER_CONST_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "zero_cc/types/types.h"
#include "zero_cc/types/registry.h"
#include "zero_cc/types/global_vars.h"
/* type_parser.h first: consumers of const_eval.h (ast_expr.h, init_common.h,
 * decl_local.c, ...) call type_parser's zero_cc_parse_type_name & friends
 * without including it themselves - the original header provided this
 * transitively (it included type_parser.h at the end of the file). */
#include "zero_cc/parser/type_parser.h"
/* Value representation for constant expressions (supports int, float, complex, and pointer) */
typedef enum {
    ZC_CV_INT = 0,
    ZC_CV_FLOAT = 1,
    ZC_CV_COMPLEX = 2,
    ZC_CV_PTR = 3,
} ZCConstValKind;

typedef struct {
    ZCConstValKind kind;
    int64_t i;      /* low 64 bits for ZC_CV_INT (and for w128 values); host address for ZC_CV_PTR */
    double r;
    double im;
    /* GNU __int128 constant support (Sprint 109): when w128 is set the value
     * is 128 bits wide, split as (hi, i). 64-bit operands mixed into a 128-bit
     * operation are extended using signed_ (sign-extend) or zero-extended. */
    uint64_t hi;
    bool w128;
    bool signed_;
    /* CTFE pointer metadata (ZC_CV_PTR): ptr points to the element in host
     * memory (string literal, global data section, or CTFE local buffer);
     * ptr_elem_size is the pointee byte width; ptr_elem_tag is ZeroTypeTag. */
    uint8_t *ptr;
    uint32_t ptr_elem_size;
    uint8_t ptr_elem_tag;
} ZCConstVal;


ZCConstVal zc_cv_int(int64_t i);
ZCConstVal zc_cv_float(double r);
ZCConstVal zc_cv_complex(double r, double im);
ZCConstVal zc_cv_ptr(uint8_t *ptr, uint32_t elem_size, uint8_t elem_tag);
ZCConstVal zc_cv_deref(ZCConstVal p, int64_t index);
void zc_cv_store(ZCConstVal p, int64_t index, ZCConstVal val);
ZCConstVal zc_cv_promote128(ZCConstVal v);
bool zc_cv128_negative(ZCConstVal v);
ZCConstVal zc_cv128_neg(ZCConstVal v);
bool zc_cv_is_true(ZCConstVal v);
ZCConstVal zc_cv_cast(ZCConstVal v, ZeroCCType ty);
ZCConstVal zc_cv_add(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_sub(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_mul(ZCConstVal a, ZCConstVal b);
void zc_cv_udiv128(uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi, uint64_t *qlo, uint64_t *qhi, uint64_t *rlo, uint64_t *rhi);
ZCConstVal zc_cv_div(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_mod(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_eq(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_ne(ZCConstVal a, ZCConstVal b);
ZCConstVal zc_cv_cmp(ZeroCCTokType op, ZCConstVal a, ZCConstVal b);
int zero_cc_parse_vector_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_size);
int zero_cc_parse_aligned_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_align);
int zero_cc_parse_mode_attr(ZeroCCCompiler *c, size_t *pos, size_t *out_bytes);
void zero_cc_skip_mode_attr(ZeroCCCompiler *c);
int zero_cc_parse_alias_attr(ZeroCCCompiler *c, size_t *pos, const char **out_target);
bool zero_cc_tok_is_type_start(ZeroCCCompiler *c, size_t pos);
size_t zero_cc_sizeof_ident_size(ZeroCCCompiler *c, size_t *pos, int *out_vla_local, unsigned *out_vla_strip);
size_t zero_cc_sizeof_ident_align(ZeroCCCompiler *c, size_t *pos);
int64_t eval_sizeof(ZeroCCCompiler *c, size_t *pos);
int64_t eval_alignof(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_conditional(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_logical_or(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_logical_and(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitor(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitxor(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_bitand(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_equality(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_relational(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_shift(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_additive(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_multiplicative(ZeroCCCompiler *c, size_t *pos);
ZCConstVal parse_unary(ZeroCCCompiler *c, size_t *pos);
int64_t zero_cc_eval_builtin_offsetof(ZeroCCCompiler *c, size_t *pos);
bool zc_const_global_scalar(ZeroCCCompiler *c, const char *name, bool *out_is_float, int64_t *out_i, double *out_d);
ZCConstVal parse_primary(ZeroCCCompiler *c, size_t *pos);
int64_t zero_cc_eval_const_expr(ZeroCCCompiler *c);
ZCConstVal zero_cc_eval_const_expr_val(ZeroCCCompiler *c);
bool zc_typedef_dim_is_const(ZeroCCCompiler *c, size_t start);
ZCConstVal ctfe_eval_body(ZeroCCCompiler *c, size_t start, size_t end);
ZCConstVal ctfe_eval_call(ZeroCCCompiler *c, ZeroCCFunction *fn, size_t *pos);

#endif /* ZERO_CC_PARSER_CONST_EVAL_H */
