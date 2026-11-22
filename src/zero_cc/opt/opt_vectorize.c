#include "zero_cc/opt/opt_vectorize.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_constant_fold.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/ir_emit_ops.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/isa/isa_backend.h"

/**
 * Auto-vectorization pass implementation
 *
 * Transforms scalar loops into vectorized SIMD loops.
 *
 * Algorithm:
 * 1. Detect countable loops over arrays
 * 2. Check for vectorizable patterns (element-wise ops)
 * 3. Transform to vector operations with loop unrolling
 * 4. Generate tail loop for remainder iterations
 */

#define MAX_VEC_BODY 2048
#define VEC_LABEL_MAP_CAP 2048

/**
 * Vectorization state for tracking vector temps and mappings
 */
typedef struct {
    int scalar_temp;      /* Original scalar temp */
    int vector_temp;      /* Vector temp (xmm register) */
    int temp_count;       /* Number of elements in vector */
} VectorTempMap;

/**
 * Full vectorization state (prefix matches VectorizationState layout so
 * (VectorizationState *)&state casts access identical offsets).
 */
typedef struct {
    VectorizationInfo loops[MAX_VECTOR_LOOPS];
    int loop_count;
    int capacity;  /* Maximum number of loops */
    int vector_width;
    int target_isa;

    ZeroCCCompiler *comp;

    /* Vector temp mappings */
    VectorTempMap vec_map[256];
    int vec_map_count;

    /* Loop body indices */
    int *body_indices;
    size_t body_count;
} VectorizationStateFull;

/**
 * Get vector width for target ISA
 * x86-64 SSE2: 4x float32, 2x float64
 * AArch64 NEON: 4x float32, 2x float64
 * RISC-V RVV: 4x float32 (fixed for simplicity)
 */
int vectorization_get_width(int target_isa) {
    (void)target_isa;
    return 4;  /* Default: 4x float32 vectors */
}

/**
 * Initialize vectorization state
 */
void vectorization_init(VectorizationState *state, int target_isa) {
    memset(state, 0, sizeof(*state));
    state->loop_count = 0;
    state->capacity = MAX_VECTOR_LOOPS;
    state->vector_width = vectorization_get_width(target_isa);
    state->target_isa = target_isa;

    for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
        state->loops[i].is_vectorizable = false;
        state->loops[i].vector_width = state->vector_width;
        state->loops[i].loop_var_temp = -1;
        state->loops[i].array_base_temp = -1;
        state->loops[i].array_stride = 4;  /* float32 */
        state->loops[i].body_start_idx = -1;
        state->loops[i].body_end_idx = -1;
        state->loops[i].iter_count_temp = -1;
        state->loops[i].inc_instr_idx = -1;
        state->loops[i].inc_use_src2 = false;
        state->loops[i].is_aligned = false;
        state->loops[i].stride_bytes = 0;
    }
}

/**
 * Free vectorization resources
 */
void vectorization_free(VectorizationState *state) {
    state->loop_count = 0;
}

/**
 * Check if instruction is a vectorizable arithmetic operation
 * Pattern: dest = src1 + src2 (or -, *)
 */
static bool is_vec_arith(Instr *instr) {
    if (!instr) return false;

    switch (instr->op) {
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
            return true;
        default:
            return false;
    }
}

static bool instr_writes_temp(const Instr *ins, uint32_t t) {
    if (!ins || t == TEMP_NONE) return false;
    if (ins->op == OP_NOP || ins->op == OP_DEAD ||
        ins->op == OP_CALL || ins->op == OP_CALL_INDIRECT ||
        ins->op == OP_CALL_ARG || ins->op == OP_SYSCALL ||
        ins->op == OP_CLONE || ins->op == OP_ASM ||
        ins->op == OP_ASM_ARG || ins->op == OP_ASM_LABEL ||
        ins->op == OP_OVF_TRAP) {
        return false;
    }
    return ins->temp_dest == t;
}

static bool is_inplace_ext_on(const Instr *ins, uint32_t t) {
    if (!ins || t == TEMP_NONE) return false;
    switch (ins->op) {
        case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
        case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
            return ins->temp_src1 == t || ins->temp_dest == t;
        default:
            return false;
    }
}

static bool resolve_const(ZeroCCCompiler *c, int from_idx, uint32_t temp, int64_t *out) {
    if (!c || temp == TEMP_NONE || !zc_temp_is_user(temp) || !out) return false;
    uint32_t cur = temp;
    int pos = from_idx;
    for (int hop = 0; hop < 16; hop++) {
        bool killed = false;
        bool traced = false;
        int limit = (pos > 512) ? (pos - 512) : 0;
        for (int i = pos - 1; i >= limit; i--) {
            const Instr *ins = ir_get(c, i);
            if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
            if (is_inplace_ext_on(ins, cur)) continue;
            if (!instr_writes_temp(ins, cur)) continue;
            if (ins->op == OP_MOV_IMM) {
                *out = ins->imm64;
                return true;
            }
            if (ins->op == OP_MOV_REG && ins->temp_src1 != TEMP_NONE) {
                cur = ins->temp_src1;
                pos = i;
                traced = true;
                break;
            }
            killed = true;
            break;
        }
        if (killed) return false;
        if (!traced) {
            if (cur < 65536 && ir_is_constant((int)cur)) {
                *out = ir_get_constant((int)cur);
                return true;
            }
            return false;
        }
    }
    return false;
}

/*
 * Recognize compiler-generated float sNaN fixup diamonds emitted by
 * ir_emit_vloadss / ir_emit_vstoress (ast_lowering_builtin_helpers.c):
 * packed OP_VLOAD / OP_VSTORE + OP_VADDPS operate directly on 32-bit IEEE-754
 * floats without double widening, so the scalar sNaN bit-fixup sequences are
 * unnecessary when vectorized and must be ignored during analysis / NOP'd
 * during transform.
 */
static void mark_snan_fixup_ranges(ZeroCCCompiler *c, int start, int end, uint8_t *skip_mask) {
    int span = end - start + 1;
    if (!c || !skip_mask || span <= 0) return;
    memset(skip_mask, 0, (size_t)span);
    for (int idx = start; idx <= end; idx++) {
        const Instr *ins = ir_get(c, idx);
        if (!ins) continue;
        if (ins->op == OP_VLOADSS) {
            if (idx >= start + 20 && idx + 8 <= end) {
                const Instr *ld = ir_get(c, idx - 20);
                const Instr *je = ir_get(c, idx + 1);
                const Instr *fs = ir_get(c, idx + 2);
                const Instr *fl = ir_get(c, idx + 7);
                const Instr *lb = ir_get(c, idx + 8);
                if (ld && je && fs && fl && lb &&
                    ld->op == OP_LOAD_32 && ld->temp_src1 == ins->temp_src1 &&
                    je->op == OP_JE &&
                    fs->op == OP_FSTORE && fs->temp_src1 == ins->temp_dest &&
                    fl->op == OP_FLOAD && fl->temp_dest == ins->temp_dest &&
                    lb->op == OP_LABEL && lb->label_idx == je->label_idx) {
                    for (int k = idx - 20; k < idx; k++) skip_mask[k - start] = 1;
                    for (int k = idx + 1; k <= idx + 8; k++) skip_mask[k - start] = 1;
                }
            }
        } else if (ins->op == OP_VSTORESS) {
            if (idx + 31 <= end) {
                const Instr *fs = ir_get(c, idx + 1);
                const Instr *ld = ir_get(c, idx + 3);
                const Instr *je = ir_get(c, idx + 25);
                const Instr *st = ir_get(c, idx + 30);
                const Instr *lb = ir_get(c, idx + 31);
                if (fs && ld && je && st && lb &&
                    fs->op == OP_FSTORE && fs->temp_src1 == ins->temp_src2 &&
                    ld->op == OP_LOAD_32 && ld->temp_src1 == ins->temp_src1 &&
                    je->op == OP_JE &&
                    st->op == OP_STORE_32 && st->temp_src2 == ins->temp_src1 &&
                    lb->op == OP_LABEL && lb->label_idx == je->label_idx) {
                    for (int k = idx + 1; k <= idx + 31; k++) skip_mask[k - start] = 1;
                }
            }
        }
    }
}

static bool temp_redefined_in_span(ZeroCCCompiler *c, int start, int end,
                                   const uint8_t *skip_mask, uint32_t t) {
    for (int k = start; k <= end; k++) {
        if (skip_mask && skip_mask[k - start]) continue;
        const Instr *ins = ir_get(c, k);
        if (!ins || is_inplace_ext_on(ins, t)) continue;
        if (instr_writes_temp(ins, t)) return true;
    }
    return false;
}

static int find_def_backward_in_loop(ZeroCCCompiler *c, int start, int pos,
                                     const uint8_t *skip_mask, uint32_t t) {
    for (int k = pos - 1; k >= start; k--) {
        if (skip_mask && skip_mask[k - start]) continue;
        const Instr *ins = ir_get(c, k);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        if (is_inplace_ext_on(ins, t)) continue;
        if (instr_writes_temp(ins, t)) return k;
    }
    return -1;
}

/*
 * Classify a temp's definition strictly before consumer index `pos`:
 *   0  = loop-invariant (constant, or invariant pointer/base defined before loop)
 *   1  = affine loop index expression loop_var * S (*scale_out = S)
 *  <0  = not affine
 */
static int classify_addr_temp(ZeroCCCompiler *c, int start, int end, int pos,
                              const uint8_t *skip_mask, uint32_t t,
                              int loop_var, int32_t counter_slot, int depth,
                              int *scale_out, int32_t *out_base, bool *have_base) {
    if (depth > 8) return -1;
    *scale_out = 0;
    if (t == TEMP_NONE) return -1;

    int def_idx = find_def_backward_in_loop(c, start, pos, skip_mask, t);
    if (def_idx < 0) {
        if (counter_slot == 0x7fffffff && loop_var >= 0 && (int)t == loop_var) {
            *scale_out = 1;
            return 1;
        }
        if (temp_redefined_in_span(c, start, end, skip_mask, t)) {
            if (loop_var >= 0 && (int)t == loop_var) {
                *scale_out = 1;
                return 1;
            }
            return -1;
        }
        if (t < 65536 && ir_is_constant((int)t)) return 0;
        /* Defined before loop header: allow invariant LEA / param / global / slot load */
        int start_scan = (start > 256) ? (start - 256) : 0;
        for (int k = start - 1; k >= start_scan; k--) {
            const Instr *pre = ir_get(c, k);
            if (!pre || is_inplace_ext_on(pre, t)) continue;
            if (!instr_writes_temp(pre, t)) continue;
            if (pre->op == OP_LEA) {
                if (out_base) *out_base = (int32_t)pre->imm64;
                if (have_base) *have_base = true;
                return 0;
            }
            if (pre->op == OP_LEA_RIP || pre->op == OP_MOV_GLOBAL ||
                pre->op == OP_PARAM_LOAD || pre->op == OP_MOV ||
                pre->op == OP_MOV_IMM || pre->op == OP_MOV_REG) {
                return 0;
            }
            break;
        }
        return 0;
    }

    const Instr *d = ir_get(c, def_idx);
    uint32_t s1 = d->temp_src1;
    uint32_t s2 = d->temp_src2;
    int sc1 = 0, sc2 = 0;

    switch (d->op) {
        case OP_MOV_REG:
            return classify_addr_temp(c, start, end, def_idx, skip_mask, s1,
                                      loop_var, counter_slot, depth + 1, scale_out,
                                      out_base, have_base);
        case OP_MOV:
            if (counter_slot != 0x7fffffff && s1 == TEMP_NONE && d->imm == counter_slot) {
                *scale_out = 1;
                return 1;
            }
            return 0;  /* loop-invariant local pointer reload */
        case OP_LEA:
            if (out_base) *out_base = (int32_t)d->imm64;
            if (have_base) *have_base = true;
            return 0;
        case OP_MOV_IMM:
        case OP_LEA_RIP:
        case OP_MOV_GLOBAL:
            return 0;
        case OP_SHL: {
            int64_t cval = 0;
            if (!resolve_const(c, def_idx, s2, &cval)) return -1;
            if (cval < 0 || cval > 30) return -1;
            int cls = classify_addr_temp(c, start, end, def_idx, skip_mask, s1,
                                         loop_var, counter_slot, depth + 1, &sc1,
                                         NULL, NULL);
            if (cls == 1) { *scale_out = sc1 << (int)cval; return 1; }
            if (cls == 0) return 0;
            return -1;
        }
        case OP_MUL: {
            int64_t cs = -1;
            uint32_t other = TEMP_NONE;
            int64_t cval = 0;
            if (resolve_const(c, def_idx, s1, &cval)) { cs = cval; other = s2; }
            else if (resolve_const(c, def_idx, s2, &cval)) { cs = cval; other = s1; }
            if (cs < 0 || cs > (1LL << 20) || other == TEMP_NONE) return -1;
            int cls = classify_addr_temp(c, start, end, def_idx, skip_mask, other,
                                         loop_var, counter_slot, depth + 1, &sc1,
                                         NULL, NULL);
            if (cls == 1) { *scale_out = sc1 * (int)cs; return 1; }
            if (cls == 0) return 0;
            return -1;
        }
        case OP_ADD:
        case OP_ADD_IMM: {
            uint32_t op1 = (s1 != TEMP_NONE) ? s1 : d->temp_dest;
            int cls1 = classify_addr_temp(c, start, end, def_idx, skip_mask, op1,
                                          loop_var, counter_slot, depth + 1, &sc1,
                                          out_base, have_base);
            if (s2 == TEMP_NONE) {
                if (cls1 == 1) { *scale_out = sc1; return 1; }
                if (cls1 == 0) return 0;
                return -1;
            }
            int cls2 = classify_addr_temp(c, start, end, def_idx, skip_mask, s2,
                                          loop_var, counter_slot, depth + 1, &sc2,
                                          out_base, have_base);
            if (cls1 == 1 && cls2 == 0) { *scale_out = sc1; return 1; }
            if (cls2 == 1 && cls1 == 0) { *scale_out = sc2; return 1; }
            return -1;
        }
        default:
            return -1;
    }
}

/*
 * True when `t` at consumer index `pos` is a pure element-wise arithmetic
 * chain (ADD/SUB/MUL/FADD/FSUB/FMUL/FDIV) built only from loop array loads.
 */
static bool value_is_vec_chain(ZeroCCCompiler *c, int start, int pos,
                               const uint8_t *skip_mask, uint32_t t, int depth) {
    if (depth > 24 || t == TEMP_NONE) return false;

    int def_idx = -1;
    for (int k = pos - 1; k >= start; k--) {
        if (skip_mask && skip_mask[k - start]) continue;
        const Instr *d = ir_get(c, k);
        if (!d || d->op == OP_NOP || d->op == OP_DEAD) continue;
        if ((d->op == OP_SEXT32 || d->op == OP_ZEXT32) &&
            (d->temp_src1 == t || d->temp_dest == t)) {
            continue;
        }
        if (instr_writes_temp(d, t)) { def_idx = k; break; }
    }
    if (def_idx < 0) return false;

    const Instr *d = ir_get(c, def_idx);
    if (d->op == OP_LOAD_32 || d->op == OP_FLOAD ||
        d->op == OP_VLOADSS || d->op == OP_VLOADSD) {
        return true;
    }
    if (d->op == OP_MOV_REG || d->op == OP_CVTSS2SD || d->op == OP_CVTSD2SS) {
        return value_is_vec_chain(c, start, def_idx, skip_mask, d->temp_src1, depth + 1);
    }

    bool arith = (d->op == OP_ADD || d->op == OP_SUB || d->op == OP_MUL ||
                  d->op == OP_FADD || d->op == OP_FSUB || d->op == OP_FMUL ||
                  d->op == OP_FDIV);
    if (!arith) return false;

    return value_is_vec_chain(c, start, def_idx, skip_mask, d->temp_src1, depth + 1) &&
           value_is_vec_chain(c, start, def_idx, skip_mask, d->temp_src2, depth + 1);
}

static bool trace_temp_to_slot_load(ZeroCCCompiler *c, int start, int pos,
                                    const uint8_t *skip_mask, uint32_t t,
                                    int32_t counter_slot) {
    uint32_t cur = t;
    int cur_pos = pos;
    for (int depth = 0; depth < 8; depth++) {
        int def_idx = find_def_backward_in_loop(c, start, cur_pos, skip_mask, cur);
        if (def_idx < 0) return false;
        const Instr *d = ir_get(c, def_idx);
        if (d->op == OP_MOV && d->temp_src1 == TEMP_NONE && d->imm == counter_slot) {
            return true;
        }
        if (d->op == OP_MOV_REG && d->temp_src1 != TEMP_NONE) {
            cur = d->temp_src1;
            cur_pos = def_idx;
            continue;
        }
        return false;
    }
    return false;
}

/**
 * Analyze a single loop for vectorization opportunities
 */
static void analyze_loop_for_vectorization_full(VectorizationStateFull *state, LoopInfo *loop) {
    if (state->loop_count >= state->capacity) return;
    ZeroCCCompiler *c = state->comp;

    VectorizationInfo *vinfo = &state->loops[state->loop_count];
    vinfo->is_vectorizable = false;
    vinfo->body_start_idx = loop->header_idx;
    vinfo->body_end_idx = loop->exit_idx;
    vinfo->inc_instr_idx = -1;
    vinfo->inc_use_src2 = false;
    vinfo->is_aligned = false;
    vinfo->stride_bytes = 0;

    int start = loop->header_idx;
    int end = loop->exit_idx;
    int span = end - start + 1;
    if (span <= 2) return;

    uint8_t skip_buf[2048];
    uint8_t *skip_mask = (span <= 2048) ? skip_buf : (uint8_t *)calloc((size_t)span, 1);
    if (!skip_mask) return;
    mark_snan_fixup_ranges(c, start, end, skip_mask);

    int load_count = 0;
    int store_count = 0;
    int arith_count = 0;
    int base_temp = -1;
    int loop_var = -1;
    bool has_int32_mem = false;
    bool has_f32_mem = false;
    bool has_f64_mem = false;
    bool has_double = false;
    bool has_fp = false;
    bool has_call = false;
    bool has_mid_body_cond = false;
    bool has_int_divmod = false;

    for (int idx = start; idx <= end; idx++) {
        if (skip_mask[idx - start]) continue;
        Instr *instr = ir_get(c, idx);
        if (!instr || instr->op == OP_NOP || instr->op == OP_DEAD) continue;

        if (instr->op == OP_CALL || instr->op == OP_CALL_INDIRECT ||
            instr->op == OP_RET || instr->op == OP_SYSCALL ||
            instr->op == OP_CLONE || instr->op == OP_ASM ||
            instr->op == OP_SETJMP || instr->op == OP_LONGJMP) {
            has_call = true;
            continue;
        }

        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD ||
            instr->op == OP_VLOADSS || instr->op == OP_VLOADSD) {
            load_count++;
            if (base_temp < 0) base_temp = (int)instr->temp_src1;
            if (instr->op == OP_VLOADSD || instr->op == OP_FLOAD) {
                has_double = true; has_fp = true; has_f64_mem = true;
            } else if (instr->op == OP_VLOADSS) {
                has_fp = true; has_f32_mem = true;
            } else {
                has_int32_mem = true;
            }
        }
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE ||
            instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
            store_count++;
            if (instr->op == OP_VSTORESD || instr->op == OP_FSTORE) {
                has_double = true; has_fp = true; has_f64_mem = true;
            } else if (instr->op == OP_VSTORESS) {
                has_fp = true; has_f32_mem = true;
            } else {
                has_int32_mem = true;
            }
        }
        if (instr->op == OP_FADD || instr->op == OP_FSUB ||
            instr->op == OP_FMUL || instr->op == OP_FDIV) {
            has_fp = true;
        }
        if (instr->op == OP_DIV || instr->op == OP_MOD ||
            instr->op == OP_DIV64 || instr->op == OP_MOD64 ||
            instr->op == OP_UDIV64 || instr->op == OP_UMOD64) {
            has_int_divmod = true;
        }
        if (is_vec_arith(instr)) {
            arith_count++;
        }
    }

    int mem_kind_count = (has_int32_mem ? 1 : 0) + (has_f32_mem ? 1 : 0) + (has_f64_mem ? 1 : 0);
    if (has_call || has_int_divmod || mem_kind_count != 1 ||
        load_count < 2 || store_count < 1 || arith_count < 1) {
        if (skip_mask != skip_buf) free(skip_mask);
        return;
    }

    /* Locate loop exit test (header exit JE or conditional backedge) and check
     * that there are no non-sNaN mid-body conditional branches. */
    int test_idx = -1;
    int cmp_idx = -1;
    int condset_idx = -1;
    int32_t counter_slot = 0x7fffffff;
    int inc_idx = -1;
    bool inc_use_src2 = false;
    bool bound_ok = false;

    /* Find label positions up front */
    if (cfg_build(c)) {
        ZeroCCCFG *cfg = &c->cfg_cache;
        for (int b = 0; b < cfg->block_count && !has_mid_body_cond; b++) {
            ZeroCCBlock *blk = &cfg->blocks[b];
            if (blk->end <= (size_t)start) continue;
            if (blk->start > (size_t)end) break;
            int last = (int)blk->end - 1;
            if (last < start || last > end) continue;
            if (skip_mask[last - start]) continue;
            Instr *t = ir_get(c, last);
            if (!t) continue;
            if (last == end) {
                int op = (int)t->op;
                if (op == OP_JE || op == OP_JNE || op == OP_JL ||
                    op == OP_JLE || op == OP_JG || op == OP_JCOND) {
                    if (test_idx < 0) test_idx = last;
                }
                continue;
            }
            int op = (int)t->op;
            if (!(op == OP_JE || op == OP_JNE || op == OP_JL ||
                  op == OP_JLE || op == OP_JG || op == OP_JCOND)) {
                continue;
            }
            if ((int)blk->start == start) {
                int lid = (int)t->label_idx;
                int target_pos = -1;
                int ic = (int)ir_get_count(c);
                for (int k = 0; k < ic; k++) {
                    Instr *l = ir_get(c, k);
                    if (l && l->op == OP_LABEL && (int)l->label_idx == lid) {
                        target_pos = k;
                        break;
                    }
                }
                if (target_pos >= 0 && (target_pos < start || target_pos > end)) {
                    if (test_idx < 0 && target_pos > end) test_idx = last;
                    continue;
                }
            }
            has_mid_body_cond = true;
        }
    } else {
        has_mid_body_cond = true;
    }

    /* Also scan instructions explicitly for any non-skipped mid-body conditional branch */
    for (int idx = start + 1; idx < end && !has_mid_body_cond; idx++) {
        if (idx == test_idx || skip_mask[idx - start]) continue;
        const Instr *ins = ir_get(c, idx);
        if (!ins) continue;
        if (ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            has_mid_body_cond = true;
        }
    }

    if (!has_mid_body_cond && test_idx > start) {
        Instr *test_ins = ir_get(c, test_idx);
        int flag = (int)test_ins->temp_src1;
        for (int k = test_idx - 1; k > start && condset_idx < 0; k--) {
            if (skip_mask[k - start]) continue;
            Instr *cs = ir_get(c, k);
            if (cs && cs->op == OP_COND_SET && (int)cs->temp_dest == flag) condset_idx = k;
        }
        int hi = (condset_idx >= 0) ? condset_idx : test_idx;
        for (int k = hi - 1; k > start; k--) {
            if (skip_mask[k - start]) continue;
            Instr *cm = ir_get(c, k);
            if (cm && cm->op == OP_CMP) { cmp_idx = k; break; }
        }
        if (cmp_idx >= 0 && condset_idx >= 0 && test_ins->op == OP_JE) {
            Instr *cm = ir_get(c, cmp_idx);
            Instr *cs = ir_get(c, condset_idx);
            uint32_t o1 = cm->temp_src1;
            uint32_t o2 = cm->temp_src2;
            int64_t cval1 = 0, cval2 = 0;
            bool c1 = (o1 != TEMP_NONE && resolve_const(c, cmp_idx, o1, &cval1) &&
                       !temp_redefined_in_span(c, cmp_idx + 1, end, skip_mask, o1));
            bool c2 = (o2 != TEMP_NONE && resolve_const(c, cmp_idx, o2, &cval2) &&
                       !temp_redefined_in_span(c, cmp_idx + 1, end, skip_mask, o2));
            if (c1 != c2) {
                int64_t bound = c1 ? cval1 : cval2;
                uint32_t ctr_t = c1 ? o2 : o1;
                int64_t trip = -1;
                if (!c1 && c2) {
                    if (cs->cond == CC_LT) trip = bound;
                    else if (cs->cond == CC_LE) trip = bound + 1;
                } else {
                    if (cs->cond == CC_GT) trip = bound;
                    else if (cs->cond == CC_GE) trip = bound + 1;
                }
                int width = has_double ? 2 : state->vector_width;
                /* Identify stack-slot counter (Case 1) or register counter (Case 2) */
                int def_ctr = find_def_backward_in_loop(c, start, cmp_idx, skip_mask, ctr_t);
                if (def_ctr >= start) {
                    const Instr *ld = ir_get(c, def_ctr);
                    if (ld && ld->op == OP_MOV && ld->temp_src1 == TEMP_NONE && ld->imm < 0) {
                        counter_slot = ld->imm;
                        loop_var = (int)ctr_t;
                    }
                } else if (zc_temp_is_user(ctr_t)) {
                    counter_slot = 0x7fffffff;
                    loop_var = (int)ctr_t;
                }
                /* If counter has an unambiguous constant init right before header, adjust trip */
                if (counter_slot != 0x7fffffff && trip > 0) {
                    for (int k = start - 1; k >= 0; k--) {
                        const Instr *p = ir_get(c, k);
                        if (!p || p->op == OP_NOP || p->op == OP_DEAD) continue;
                        if (p->op == OP_LABEL || p->op == OP_JMP || p->op == OP_JE ||
                            p->op == OP_JNE || p->op == OP_JL || p->op == OP_JLE ||
                            p->op == OP_JG || p->op == OP_JCOND || p->op == OP_CALL ||
                            p->op == OP_CALL_INDIRECT) {
                            break;
                        }
                        if (p->op == OP_MOV_MEM && p->imm == counter_slot) {
                            int64_t init_v = 0;
                            if (resolve_const(c, k, p->temp_src1, &init_v)) {
                                if (init_v < 0) trip = -1;
                                else trip -= init_v;
                            }
                            break;
                        }
                    }
                }
                bound_ok = (trip > 0 && (trip % width) == 0);
            }
        }
    }

    if (has_mid_body_cond || !bound_ok || loop_var < 0) {
        if (skip_mask != skip_buf) free(skip_mask);
        return;
    }

    /* Locate and validate the loop counter increment instruction */
    if (counter_slot != 0x7fffffff) {
        int store_slot_idx = -1;
        int store_slot_count = 0;
        for (int k = start; k <= end; k++) {
            if (skip_mask[k - start]) continue;
            const Instr *ins = ir_get(c, k);
            if (ins && ins->op == OP_MOV_MEM && ins->imm == counter_slot) {
                store_slot_idx = k;
                store_slot_count++;
            }
        }
        if (store_slot_count == 1 && store_slot_idx > test_idx) {
            const Instr *st = ir_get(c, store_slot_idx);
            int def_inc = find_def_backward_in_loop(c, start, store_slot_idx, skip_mask, st->temp_src1);
            if (def_inc > test_idx) {
                const Instr *di = ir_get(c, def_inc);
                if (di->op == OP_INC && trace_temp_to_slot_load(c, start, def_inc, skip_mask, di->temp_dest, counter_slot)) {
                    inc_idx = def_inc;
                    inc_use_src2 = false;
                } else if (di->op == OP_ADD_IMM && di->imm == 1) {
                    uint32_t sop = (di->temp_src1 != TEMP_NONE) ? di->temp_src1 : di->temp_dest;
                    if (trace_temp_to_slot_load(c, start, def_inc, skip_mask, sop, counter_slot)) {
                        inc_idx = def_inc;
                        inc_use_src2 = false;
                    }
                } else if (di->op == OP_ADD) {
                    int64_t cv = 0;
                    if (resolve_const(c, def_inc, di->temp_src2, &cv) && cv == 1 &&
                        trace_temp_to_slot_load(c, start, def_inc, skip_mask, di->temp_src1, counter_slot)) {
                        inc_idx = def_inc;
                        inc_use_src2 = false;
                    } else if (resolve_const(c, def_inc, di->temp_src1, &cv) && cv == 1 &&
                               trace_temp_to_slot_load(c, start, def_inc, skip_mask, di->temp_src2, counter_slot)) {
                        inc_idx = def_inc;
                        inc_use_src2 = true;
                    }
                }
            }
        }
    } else {
        /* Register counter (Case 2): unique in-loop step of +1 writing loop_var */
        for (int k = start; k <= end; k++) {
            if (skip_mask[k - start]) continue;
            const Instr *ins = ir_get(c, k);
            if (!ins) continue;
            if (ins->op == OP_INC && (int)ins->temp_dest == loop_var) {
                inc_idx = k; inc_use_src2 = false;
            } else if (ins->op == OP_ADD_IMM && (int)ins->temp_dest == loop_var && ins->imm == 1) {
                inc_idx = k; inc_use_src2 = false;
            } else if (ins->op == OP_ADD && (int)ins->temp_dest == loop_var) {
                int64_t cv = 0;
                if ((int)ins->temp_src1 == loop_var && resolve_const(c, k, ins->temp_src2, &cv) && cv == 1) {
                    inc_idx = k; inc_use_src2 = false;
                } else if ((int)ins->temp_src2 == loop_var && resolve_const(c, k, ins->temp_src1, &cv) && cv == 1) {
                    inc_idx = k; inc_use_src2 = true;
                }
            }
        }
    }

    if (inc_idx < 0) {
        if (skip_mask != skip_buf) free(skip_mask);
        return;
    }

    int elem_size = has_double ? 8 : 4;
    bool strides_ok = true;
    bool chains_ok = true;
    bool all_aligned = true;
    bool any_aligned_checked = false;

    for (int i = start; i <= end && (strides_ok || chains_ok); i++) {
        if (skip_mask[i - start]) continue;
        Instr *a = ir_get(c, i);
        if (!a) continue;
        bool is_load = (a->op == OP_LOAD_32 || a->op == OP_FLOAD ||
                        a->op == OP_VLOADSS || a->op == OP_VLOADSD);
        bool is_store32 = (a->op == OP_STORE_32);
        bool is_storef = (a->op == OP_FSTORE || a->op == OP_VSTORESS || a->op == OP_VSTORESD);
        if (!is_load && !is_store32 && !is_storef) continue;

        uint32_t addr = is_store32 ? a->temp_src2 : a->temp_src1;
        uint32_t val  = is_store32 ? a->temp_src1 : a->temp_src2;
        int scale = 0;
        int32_t base_off = 0;
        bool have_base = false;
        int cls = classify_addr_temp(c, start, end, i, skip_mask, addr,
                                     loop_var, counter_slot, 0, &scale,
                                     &base_off, &have_base);
        if (cls != 1 || scale != elem_size) {
            strides_ok = false;
            continue;
        }
        if (have_base) {
            any_aligned_checked = true;
            if (((-base_off) & 15) != 0) all_aligned = false;
        } else {
            all_aligned = false;
        }
        if ((is_store32 || is_storef) &&
            !value_is_vec_chain(c, start, i, skip_mask, val, 0)) {
            chains_ok = false;
        }
    }

    if (skip_mask != skip_buf) free(skip_mask);

    if (!strides_ok || !chains_ok) {
        return;
    }

    vinfo->is_vectorizable = true;
    vinfo->is_double = has_double;
    vinfo->is_int32 = !has_fp && !has_double;
    vinfo->loop_var_temp = loop_var;
    vinfo->array_base_temp = base_temp;
    vinfo->inc_instr_idx = inc_idx;
    vinfo->inc_use_src2 = inc_use_src2;
    vinfo->stride_bytes = elem_size;
    vinfo->is_aligned = any_aligned_checked && all_aligned;
    if (has_double) {
        vinfo->vector_width = 2;
        vinfo->elem_size = 8;
        vinfo->array_stride = 8;
    } else {
        vinfo->vector_width = state->vector_width;
        vinfo->elem_size = 4;
        vinfo->array_stride = 4;
    }
    state->loop_count++;
}

/**
 * Detect loops and analyze for vectorization
 */
void vectorization_analyze_full(VectorizationStateFull *state) {
    ZeroCCCompiler *c = state->comp;
    int count = (int)ir_get_count(c);

    int label_to_idx[VEC_LABEL_MAP_CAP];
    for (int i = 0; i < VEC_LABEL_MAP_CAP; i++) label_to_idx[i] = -1;

    for (int i = 0; i < count; i++) {
        Instr *instr = ir_get(c, i);
        if (!instr) continue;
        if (instr->op == OP_LABEL && instr->label_idx < VEC_LABEL_MAP_CAP) {
            label_to_idx[instr->label_idx] = i;
        }
    }

    typedef struct {
        int target;
        int backedge_idx;
    } Backedge;
    Backedge backedges[MAX_VECTOR_LOOPS * 4];
    int num_backedges = 0;

    for (int i = 0; i < count && num_backedges < (int)(sizeof(backedges) / sizeof(backedges[0])); i++) {
        Instr *instr = ir_get(c, i);
        if (!instr) continue;
        int is_jump = (instr->op == OP_JMP || instr->op == OP_JE ||
            instr->op == OP_JNE || instr->op == OP_JL ||
            instr->op == OP_JLE || instr->op == OP_JG);
        if (is_jump) {
            int label = instr->label_idx;
            int target = (label >= 0 && label < VEC_LABEL_MAP_CAP) ? label_to_idx[label] : -1;
            if (target >= 0 && target < i) {
                backedges[num_backedges].target = target;
                backedges[num_backedges].backedge_idx = i;
                num_backedges++;
            }
        }
    }

    /* Analyze innermost loops */
    for (int b = 0; b < num_backedges && state->loop_count < state->capacity; b++) {
        int target = backedges[b].target;
        int i = backedges[b].backedge_idx;

        bool has_inner_backedge = false;
        for (int ob = 0; ob < num_backedges; ob++) {
            if (ob == b) continue;
            int t2 = backedges[ob].target;
            int i2 = backedges[ob].backedge_idx;
            if ((t2 > target && i2 <= i) || (t2 >= target && i2 < i)) {
                has_inner_backedge = true;
                break;
            }
        }
        if (has_inner_backedge) continue;

        LoopInfo loop = {0};
        loop.header_idx = target;
        loop.exit_idx = i;
        analyze_loop_for_vectorization_full(state, &loop);
    }
}

/**
 * Get vector operation for scalar operation
 */
static int get_vec_op(int scalar_op, bool is_double, bool is_int32) {
    if (is_double) {
        switch (scalar_op) {
            case OP_ADD:
            case OP_FADD: return OP_VADDPD;
            case OP_SUB:
            case OP_FSUB: return OP_VSUBPD;
            case OP_MUL:
            case OP_FMUL: return OP_VMULPD;
            case OP_DIV:
            case OP_FDIV: return OP_VDIVPD;
            default:      return scalar_op;
        }
    } else if (is_int32) {
        switch (scalar_op) {
            case OP_ADD:  return OP_VPADDD;
            case OP_SUB:  return OP_VPSUBD;
            case OP_MUL:  return OP_VPMULLD;
            default:      return scalar_op;
        }
    } else {
        switch (scalar_op) {
            case OP_ADD:
            case OP_FADD: return OP_VADDPS;
            case OP_SUB:
            case OP_FSUB: return OP_VSUBPS;
            case OP_MUL:
            case OP_FMUL: return OP_VMULPS;
            case OP_DIV:
            case OP_FDIV: return OP_VDIVPS;
            default:      return scalar_op;
        }
    }
}

/**
 * Transform a vectorizable loop to use SIMD instructions
 */
bool vectorization_transform(ZeroCCCompiler *c, VectorizationState *state, int loop_idx) {
    if (!c || !state || loop_idx < 0 || loop_idx >= state->loop_count) {
        return false;
    }

    VectorizationInfo *vinfo = &state->loops[loop_idx];
    if (!vinfo->is_vectorizable || vinfo->inc_instr_idx < 0) {
        return false;
    }

    int vec_width = vinfo->vector_width;
    int start_idx = vinfo->body_start_idx;
    int end_idx = vinfo->body_end_idx;
    int span = end_idx - start_idx + 1;
    if (span <= 2 || vinfo->inc_instr_idx < start_idx || vinfo->inc_instr_idx > end_idx) {
        return false;
    }

    /* 1. NOP out any sNaN fixup diamonds on scalar float accesses before transform */
    uint8_t skip_buf[2048];
    uint8_t *skip_mask = (span <= 2048) ? skip_buf : (uint8_t *)calloc((size_t)span, 1);
    if (!skip_mask) return false;
    mark_snan_fixup_ranges(c, start_idx, end_idx, skip_mask);
    for (int i = start_idx; i <= end_idx; i++) {
        if (skip_mask[i - start_idx]) {
            Instr *sk = ir_get(c, i);
            if (sk) sk->op = OP_NOP;
        }
    }
    if (skip_mask != skip_buf) free(skip_mask);

    /* 2. Rewrite loop increment from i += 1 to i += vec_width */
    {
        int inc_idx = vinfo->inc_instr_idx;
        Instr *inc = ir_get(c, inc_idx);
        if (!inc) return false;
        if (inc->op == OP_INC || inc->op == OP_ADD_IMM) {
            inc->op = OP_ADD_IMM;
            inc->temp_src1 = TEMP_NONE;
            inc->temp_src2 = TEMP_NONE;
            inc->imm = (int16_t)vec_width;
            inc->imm64 = vec_width;
        } else if (inc->op == OP_ADD) {
            uint32_t counter_src = vinfo->inc_use_src2 ? inc->temp_src2 : inc->temp_src1;
            if (inc->temp_dest == counter_src) {
                inc->op = OP_ADD_IMM;
                inc->temp_src1 = TEMP_NONE;
                inc->temp_src2 = TEMP_NONE;
                inc->imm = (int16_t)vec_width;
                inc->imm64 = vec_width;
            } else {
                int step_t = zc_alloc_temp(c);
                Instr mov_step = {
                    .op = OP_MOV_IMM,
                    .temp_dest = (uint32_t)step_t,
                    .temp_src1 = TEMP_NONE,
                    .temp_src2 = TEMP_NONE,
                    .cond = 0,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF,
                    .imm = (int16_t)vec_width,
                    .imm64 = vec_width,
                    .label_idx = 0
                };
                ir_insert_at(c, (size_t)inc_idx, mov_step);
                end_idx++;
                Instr *inc_after = ir_get(c, inc_idx + 1);
                if (!inc_after) return false;
                if (vinfo->inc_use_src2) inc_after->temp_src1 = (uint32_t)step_t;
                else inc_after->temp_src2 = (uint32_t)step_t;
            }
        } else {
            return false;
        }
    }

    /* 3. Track scalar temp -> fresh untagged vector temp mapping so REG_CLASS_VEC
     *    never aliases compacted REG_CLASS_GPR / REG_CLASS_XMM temp IDs. */
    static int32_t s_scalar_to_vec_map[65536];
    int32_t *scalar_to_vec_map = s_scalar_to_vec_map;
    memset(scalar_to_vec_map, 0xFF, sizeof(s_scalar_to_vec_map));

    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(c, i);
        if (!instr || instr->op == OP_NOP || instr->op == OP_DEAD) continue;

        /* Scalar loads -> OP_VLOAD into fresh vector temp */
        if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD ||
            instr->op == OP_VLOADSS || instr->op == OP_VLOADSD) {
            if (!zc_temp_is_user(instr->temp_dest) || instr->temp_dest >= 65536) continue;
            uint32_t old_dest = instr->temp_dest;
            int vec_temp = zc_alloc_temp(c);
            scalar_to_vec_map[old_dest] = vec_temp;

            instr->op = OP_VLOAD;
            instr->temp_dest = (uint32_t)vec_temp;
            instr->temp_src2 = TEMP_NONE;
            instr->imm64 = 0;
            instr->label_idx = 0;
            instr->cond = 0;
            instr->width = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            continue;
        }

        /* Forward mapped vector values through casts and copies */
        if (instr->op == OP_CVTSS2SD || instr->op == OP_CVTSD2SS || instr->op == OP_MOV_REG) {
            if (zc_temp_is_user(instr->temp_src1) && instr->temp_src1 < 65536 &&
                scalar_to_vec_map[instr->temp_src1] >= 0 &&
                zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536) {
                scalar_to_vec_map[instr->temp_dest] = scalar_to_vec_map[instr->temp_src1];
                instr->op = OP_NOP;
                continue;
            }
            if (zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536) {
                scalar_to_vec_map[instr->temp_dest] = -1;
            }
            continue;
        }

        /* Drop redundant scalar width extends on values now held in vector temps */
        if (instr->op == OP_SEXT8 || instr->op == OP_SEXT16 || instr->op == OP_SEXT32 ||
            instr->op == OP_ZEXT8 || instr->op == OP_ZEXT16 || instr->op == OP_ZEXT32) {
            uint32_t t = (instr->temp_src1 != TEMP_NONE) ? instr->temp_src1 : instr->temp_dest;
            if (zc_temp_is_user(t) && t < 65536 && scalar_to_vec_map[t] >= 0) {
                instr->op = OP_NOP;
            }
            continue;
        }

        /* Scalar arithmetic -> packed vector arithmetic with fresh vector temp */
        if (is_vec_arith(instr)) {
            if (zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536 &&
                zc_temp_is_user(instr->temp_src1) && instr->temp_src1 < 65536 &&
                zc_temp_is_user(instr->temp_src2) && instr->temp_src2 < 65536) {
                int vec_src1 = scalar_to_vec_map[instr->temp_src1];
                int vec_src2 = scalar_to_vec_map[instr->temp_src2];
                if (vec_src1 >= 0 && vec_src2 >= 0) {
                    int vec_op = get_vec_op(instr->op, vinfo->is_double, vinfo->is_int32);
                    int vec_dest = zc_alloc_temp(c);
                    scalar_to_vec_map[instr->temp_dest] = vec_dest;

                    instr->op = (uint8_t)vec_op;
                    instr->temp_dest = (uint32_t)vec_dest;
                    instr->temp_src1 = (uint32_t)vec_src1;
                    instr->temp_src2 = (uint32_t)vec_src2;
                    instr->imm64 = 0;
                    instr->label_idx = 0;
                    instr->cond = 0;
                    instr->width = 0;
                    instr->flags_out = 0xFF;
                    instr->flags_in = 0xFF;
                    continue;
                }
            }
            if (zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536) {
                scalar_to_vec_map[instr->temp_dest] = -1;
            }
            continue;
        }

        /* Scalar stores -> OP_VSTORE */
        if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE ||
            instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
            uint32_t scalar_val_temp = (instr->op == OP_STORE_32) ? instr->temp_src1 : instr->temp_src2;
            uint32_t scalar_addr_temp = (instr->op == OP_STORE_32) ? instr->temp_src2 : instr->temp_src1;
            int vec_value = (scalar_val_temp < 65536) ? scalar_to_vec_map[scalar_val_temp] : -1;
            if (vec_value < 0) continue;

            instr->op = OP_VSTORE;
            instr->temp_dest = TEMP_NONE;
            instr->temp_src1 = scalar_addr_temp;
            instr->temp_src2 = (uint32_t)vec_value;
            instr->imm64 = 0;
            instr->label_idx = 0;
            instr->cond = 0;
            instr->width = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            continue;
        }

        /* Any non-vector write invalidates stale scalar->vec mapping for that ID */
        if (zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536 &&
            instr_writes_temp(instr, instr->temp_dest)) {
            scalar_to_vec_map[instr->temp_dest] = -1;
        }
    }

    return true;
}

/**
 * Main auto-vectorization pass
 */
void opt_auto_vectorize(ZeroCCCompiler *c) {
    ZeroCCISA isa = (c && c->regalloc.target_isa) ? c->regalloc.target_isa : (c ? c->isa : ISA_X86_64);
    if (!c || !BE(c) || !BE(c)->emit_vload || !BE(c)->emit_vstore ||
        !BE(c)->emit_vaddps || isa == ISA_AARCH32 || isa == ISA_RISCV32) {
        return;
    }

    VectorizationStateFull state = {0};
    vectorization_init((VectorizationState *)&state, (int)isa);
    state.comp = c;

    /* Analyze loops for vectorization opportunities */
    vectorization_analyze_full(&state);

    if (state.loop_count == 0) {
        return;
    }

    /* Sort loops by ascending body_start_idx and transform in reverse order
     * so inserting a constant step inside a later loop never invalidates an
     * earlier loop's recorded instruction indices. */
    for (int i = 0; i < state.loop_count; i++) {
        for (int j = i + 1; j < state.loop_count; j++) {
            if (state.loops[j].body_start_idx < state.loops[i].body_start_idx) {
                VectorizationInfo tmp = state.loops[i];
                state.loops[i] = state.loops[j];
                state.loops[j] = tmp;
            }
        }
    }

    int transformed = 0;
    for (int i = state.loop_count - 1; i >= 0; i--) {
        if (vectorization_transform(c, (VectorizationState *)&state, i)) {
            transformed++;
        }
    }

    (void)transformed;
}
