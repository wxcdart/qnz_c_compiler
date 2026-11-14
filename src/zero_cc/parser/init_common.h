/* Shared aggregate-initializer designator engine - declaration header (Sprint 114 Tier C: implementation
 * moved to init_common.c; consumers link the single copy). */
#ifndef ZERO_CC_PARSER_INIT_COMMON_H
#define ZERO_CC_PARSER_INIT_COMMON_H

#include "zero_cc/parser/parser.h"
#include "zero_cc/parser/const_eval.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/types/emit_helpers.h"

typedef struct {
    size_t abs_off;      /* absolute byte offset of the value target */
    ZeroCCType val_type; /* type of the value target (aggregate or scalar) */
    bool is_scalar;      /* value target is a scalar leaf (grouping braces transparent) */
    size_t scalar_sz;    /* store width for a scalar value (1/2/4/8) */
    bool is_bitfield;    /* value target is a bitfield member */
    uint8_t bit_offset;  /* bit offset within the storage unit */
    uint8_t bit_width;   /* bit width of the bitfield */
    size_t local_leaf;   /* leaf index within an aggregate target (elided braces) */
    size_t leaf_idx;     /* leaf index (in the level) of the target start */
    bool in_member;      /* target is inside a struct/union member */
    size_t advance;      /* leaves consumed in this level by one value/sub-list */
    size_t arr_ctx_off;  /* start of the enclosing char array (string fills) */
    size_t arr_ctx_size; /* its size in bytes (0 = not a char-array context) */
} ZcInitTarget;

bool zc_probe_clit_init(ZeroCCCompiler *c, size_t pos, size_t *out_clit_type_pos, size_t *out_brace_pos);
ZeroCCType zc_init_member_value_type(const ZeroCCStructMember *m);
bool zc_init_member_leaf_count(const ZeroCCStructMember *m, size_t *out);
size_t zc_init_member_leaf_start(const ZeroCCType *st, size_t mi);
bool zc_init_resolve_positional(const ZeroCCType *t, size_t *cursor, size_t base_off, size_t fam_bytes, ZcInitTarget *out, const ZeroCCToken *tok, size_t tok_pos, size_t tok_count);
bool zc_init_parse_designators(ZeroCCCompiler *c, size_t *pos, const ZeroCCType *obj_type, size_t base_off, ZcInitTarget *tgt, bool *has_desig, bool *is_range, int64_t *lo, int64_t *hi, size_t *step);
double zc_fp_negate(double v);
double zc_eval_const_fp_multiplicative(ZeroCCCompiler *c);
double zc_eval_const_fp_additive(ZeroCCCompiler *c);
double zero_cc_eval_const_expr_float(ZeroCCCompiler *c);
int zc_eval_const_cx_multiplicative(ZeroCCCompiler *c, double *re, double *im);
int zc_eval_const_cx_additive(ZeroCCCompiler *c, double *re, double *im);
int zc_eval_const_cx_primary(ZeroCCCompiler *c, double *re, double *im);
int zero_cc_eval_const_expr_complex(ZeroCCCompiler *c, double *re, double *im);
/* C99 6.7.8p12 / C23 6.7.10: for an UNSIZED array whose initializer is a
 * brace list, a top-level `[N]` designator forces the inferred bound to at
 * least N+1 (element counting alone under-infers `int a[] = {[3]=7}`).
 * Pure token scan of the brace list starting at brace_pos (must be TK_LBRACE):
 * returns the max top-level designator index (range HI for `[lo ... hi]`),
 * or -1 when the list has no top-level index designator. */
int64_t zc_designator_max_index(ZeroCCCompiler *c, size_t brace_pos);
size_t zc_measure_fam_init_bytes(ZeroCCCompiler *c, const ZeroCCType *st);
bool zc_probe_cast_amp(ZeroCCCompiler *c);
void zero_cc_emit_brace_init_global(ZeroCCCompiler *c, ZeroCCType obj_type, uint8_t *buf, size_t buf_off, size_t buf_cap, size_t data_patch_off, size_t *cursor, size_t fam_bytes);
bool zero_cc_static_local_register(ZeroCCCompiler *c, ZeroCCSym *sym);
bool zero_cc_static_local_patch_init(ZeroCCCompiler *c, ZeroCCSym *sym);

#endif /* ZERO_CC_PARSER_INIT_COMMON_H */
