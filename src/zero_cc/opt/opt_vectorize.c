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
#define VEC_PEEL_MARKER 0x5E5E

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
    return (int)zero_cc_get_target_traits((ZeroCCISA)target_isa)->default_vec_width_f32;
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
        state->loops[i].needs_peel = false;
        state->loops[i].v_bound = 0;
        state->loops[i].test_instr_idx = -1;
        state->loops[i].cmp_instr_idx = -1;
        state->loops[i].bound_is_src2 = false;
        state->loops[i].is_scalable = false;
        state->loops[i].is_rvv = false;
        state->loops[i].is_sve = false;
        state->loops[i].bound_temp = TEMP_NONE;
        state->loops[i].bound_slot = 0x7fffffff;
        state->loops[i].counter_slot = 0x7fffffff;
        state->loops[i].has_strided_access = false;
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
        case OP_AND:
        case OP_OR:
        case OP_XOR:
        case OP_FADD:
        case OP_FSUB:
        case OP_FMUL:
        case OP_FDIV:
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

static bool resolve_const_at(ZeroCCCompiler *c, int from_idx, uint32_t temp, int64_t *out, int *def_idx_out) {
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
                if (def_idx_out) *def_idx_out = i;
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
                if (def_idx_out) *def_idx_out = -1;
                return true;
            }
            return false;
        }
    }
    return false;
}

static bool resolve_const(ZeroCCCompiler *c, int from_idx, uint32_t temp, int64_t *out) {
    return resolve_const_at(c, from_idx, temp, out, NULL);
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

static bool is_temp_or_slot_invariant_bound(ZeroCCCompiler *c, int start, int end, int cmp_idx,
                                            const uint8_t *skip_mask, uint32_t t,
                                            int32_t *out_slot) {
    if (t == TEMP_NONE) return false;
    if (out_slot) *out_slot = 0x7fffffff;
    if (!temp_redefined_in_span(c, start, end, skip_mask, t)) {
        return true;
    }
    int def_idx = find_def_backward_in_loop(c, start, cmp_idx, skip_mask, t);
    if (def_idx >= start) {
        const Instr *ld = ir_get(c, def_idx);
        if (ld && ld->op == OP_MOV && ld->temp_src1 == TEMP_NONE && ld->imm < 0) {
            int32_t slot = ld->imm;
            bool written = false;
            for (int k = start; k <= end; k++) {
                if (skip_mask && skip_mask[k - start]) continue;
                const Instr *ins = ir_get(c, k);
                if (ins && ins->op == OP_MOV_MEM && ins->imm == slot) {
                    written = true;
                    break;
                }
            }
            if (!written) {
                if (out_slot) *out_slot = slot;
                return true;
            }
        }
    }
    return false;
}

static int classify_addr_temp(ZeroCCCompiler *c, int start, int end, int pos,
                              const uint8_t *skip_mask, uint32_t t,
                              int loop_var, int32_t counter_slot, int depth,
                              int *scale_out, int32_t *out_base, bool *have_base);

/*
 * Detect indexed memory address pattern: addr = base + (idx << scale_shift) or base + (idx * scale)
 * where base is loop-invariant (cls == 0) and idx was loaded from an affine array load in the loop.
 * Returns true if indexed access, with out_base_temp, out_idx_temp, out_scale.
 */
static bool is_indexed_add(ZeroCCCompiler *c, int start, int end, int add_idx,
                           const uint8_t *skip_mask, int loop_var, int32_t counter_slot,
                           int *out_base_temp, int *out_idx_temp, int *out_scale) {
    if (add_idx < 0) return false;
    const Instr *d = ir_get(c, add_idx);
    if (!d || d->op != OP_ADD) return false;
    uint32_t op1 = d->temp_src1;
    uint32_t op2 = d->temp_src2;
    if (op1 == TEMP_NONE || op2 == TEMP_NONE) return false;

    /* Try (op1=base, op2=offset) and (op2=base, op1=offset) */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t base = (pass == 0) ? op1 : op2;
        uint32_t offset = (pass == 0) ? op2 : op1;

        int sc_base = 0;
        int cls_base = classify_addr_temp(c, start, end, add_idx, skip_mask, base,
                                          loop_var, counter_slot, 0, &sc_base, NULL, NULL);
        if (cls_base != 0) continue;

        /* Analyze offset */
        int scale = 0;
        uint32_t cand = offset;

        int def_off = find_def_backward_in_loop(c, start, add_idx, skip_mask, offset);
        if (def_off >= 0) {
            const Instr *oi = ir_get(c, def_off);
            if (oi->op == OP_SHL) {
                int64_t sh = 0;
                if (resolve_const(c, def_off, oi->temp_src2, &sh) && sh >= 0 && sh <= 4) {
                    scale = 1 << (int)sh;
                    cand = oi->temp_src1;
                }
            } else if (oi->op == OP_MUL) {
                int64_t m = 0;
                if (resolve_const(c, def_off, oi->temp_src2, &m) && m > 0 && m <= 16) {
                    scale = (int)m;
                    cand = oi->temp_src1;
                } else if (resolve_const(c, def_off, oi->temp_src1, &m) && m > 0 && m <= 16) {
                    scale = (int)m;
                    cand = oi->temp_src2;
                }
            }
        }

        if (scale == 0) continue;

        /* Trace candidate backwards past in-place SEXT/ZEXT and copies */
        uint32_t cur = cand;
        int cur_pos = add_idx;
        for (int depth = 0; depth < 8; depth++) {
            int dc = find_def_backward_in_loop(c, start, cur_pos, skip_mask, cur);
            if (dc < 0) break;
            const Instr *ci = ir_get(c, dc);
            if (!ci) break;
            if (ci->op == OP_SEXT8 || ci->op == OP_SEXT16 || ci->op == OP_SEXT32 ||
                ci->op == OP_ZEXT8 || ci->op == OP_ZEXT16 || ci->op == OP_ZEXT32) {
                if (ci->temp_src1 != TEMP_NONE && ci->temp_src1 != cur) {
                    cur = ci->temp_src1;
                }
                cur_pos = dc;
                continue;
            }
            if (ci->op == OP_MOV_REG && ci->temp_src1 != TEMP_NONE) {
                cur = ci->temp_src1;
                cur_pos = dc;
                continue;
            }
            break;
        }

        /* Verify that cur was loaded from an array in the loop */
        int def_load = find_def_backward_in_loop(c, start, cur_pos, skip_mask, cur);
        if (def_load >= 0) {
            const Instr *li = ir_get(c, def_load);
            if (li->op == OP_LOAD_32 || li->op == OP_FLOAD ||
                li->op == OP_VLOADSS || li->op == OP_VLOADSD) {
                int sc_load = 0;
                int cls_load = classify_addr_temp(c, start, end, def_load, skip_mask, li->temp_src1,
                                                  loop_var, counter_slot, 0, &sc_load, NULL, NULL);
                if (cls_load == 1) {
                    if (out_base_temp) *out_base_temp = (int)base;
                    if (out_idx_temp) *out_idx_temp = (int)li->temp_dest;
                    if (out_scale) *out_scale = scale;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool get_indexed_info(ZeroCCCompiler *c, int start, int end, int pos,
                             const uint8_t *skip_mask, uint32_t addr,
                             int loop_var, int32_t counter_slot,
                             int *out_base_temp, int *out_idx_temp, int *out_scale) {
    if (addr == TEMP_NONE) return false;
    int def_idx = find_def_backward_in_loop(c, start, pos, skip_mask, addr);
    if (def_idx < 0) return false;
    const Instr *d = ir_get(c, def_idx);
    if (!d) return false;

    if (d->op == OP_MOV_REG && d->temp_src1 != TEMP_NONE) {
        return get_indexed_info(c, start, end, def_idx, skip_mask, d->temp_src1,
                                loop_var, counter_slot, out_base_temp, out_idx_temp, out_scale);
    }
    return is_indexed_add(c, start, end, def_idx, skip_mask, loop_var, counter_slot,
                          out_base_temp, out_idx_temp, out_scale);
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
            int b_t = -1, i_t = -1, sc_idx = 0;
            if (is_indexed_add(c, start, end, def_idx, skip_mask, loop_var, counter_slot, &b_t, &i_t, &sc_idx)) {
                *scale_out = sc_idx;
                return 2;
            }
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
        if ((d->op == OP_SEXT8 || d->op == OP_SEXT16 || d->op == OP_SEXT32 ||
             d->op == OP_ZEXT8 || d->op == OP_ZEXT16 || d->op == OP_ZEXT32) &&
            (d->temp_src1 == t || d->temp_dest == t)) {
            if (d->temp_src1 != TEMP_NONE && d->temp_dest != d->temp_src1) {
                return value_is_vec_chain(c, start, k, skip_mask, d->temp_src1, depth + 1);
            }
            continue;
        }
        if (instr_writes_temp(d, t)) { def_idx = k; break; }
    }
    if (def_idx < 0) return false;

    const Instr *d = ir_get(c, def_idx);
    if (d->op == OP_LOAD_BYTE || d->op == OP_LOAD_16 ||
        d->op == OP_LOAD_32 || d->op == OP_FLOAD ||
        d->op == OP_VLOADSS || d->op == OP_VLOADSD) {
        return true;
    }
    if (d->op == OP_MOV_REG || d->op == OP_CVTSS2SD || d->op == OP_CVTSD2SS) {
        return value_is_vec_chain(c, start, def_idx, skip_mask, d->temp_src1, depth + 1);
    }
    if (d->op == OP_SEXT8 || d->op == OP_SEXT16 || d->op == OP_SEXT32 ||
        d->op == OP_ZEXT8 || d->op == OP_ZEXT16 || d->op == OP_ZEXT32) {
        return value_is_vec_chain(c, start, def_idx, skip_mask, d->temp_src1, depth + 1);
    }

    bool arith = (d->op == OP_ADD || d->op == OP_SUB || d->op == OP_MUL ||
                  d->op == OP_AND || d->op == OP_OR || d->op == OP_XOR ||
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
 * Detect horizontal vector reduction in loop
 */
static bool detect_loop_reduction(
    ZeroCCCompiler *c,
    int start, int end,
    const uint8_t *skip_mask,
    int loop_var,
    int32_t counter_slot,
    int *out_arr_load_idx,
    int *out_red_load_idx,
    int *out_red_store_idx,
    int *out_red_arith_idx,
    int32_t *out_red_slot,
    int *out_red_temp,
    int *out_red_op,
    bool *out_is_fp,
    bool *out_is_double
) {
    int arr_load_idx = -1;
    int arr_load_count = 0;
    int elem_size = 4;
    (void)elem_size;
    bool is_fp = false;
    bool is_double = false;

    /* 1. Find the single array load and check for NO array stores */
    for (int i = start; i <= end; i++) {
        if (skip_mask && skip_mask[i - start]) continue;
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

        bool is_load = (ins->op == OP_LOAD_32 || ins->op == OP_FLOAD ||
                        ins->op == OP_VLOADSS || ins->op == OP_VLOADSD);
        bool is_store = (ins->op == OP_STORE_32 || ins->op == OP_FSTORE ||
                         ins->op == OP_VSTORESS || ins->op == OP_VSTORESD);

        if (is_load || is_store) {
            uint32_t addr = (ins->op == OP_STORE_32) ? ins->temp_src2 : ins->temp_src1;
            int scale = 0;
            int32_t base_off = 0;
            bool have_base = false;
            int cls = classify_addr_temp(c, start, end, i, skip_mask, addr,
                                         loop_var, counter_slot, 0, &scale,
                                         &base_off, &have_base);
            if (cls == 1) {
                if (is_store) {
                    /* Array store found - not a pure reduction loop */
                    return false;
                }
                bool is_dbl = (ins->op == OP_VLOADSD || ins->op == OP_FLOAD);
                int esz_req = is_dbl ? 8 : 4;
                if (scale <= 0 || (scale % esz_req) != 0) return false;
                arr_load_idx = i;
                arr_load_count++;
                elem_size = esz_req;
                if (is_dbl) {
                    is_double = true;
                    is_fp = true;
                } else if (ins->op == OP_VLOADSS) {
                    is_fp = true;
                }
            }
        }
    }

    if (arr_load_count != 1 || arr_load_idx < 0) return false;

    Instr *arr_ins = ir_get(c, arr_load_idx);
    uint32_t arr_dest = arr_ins->temp_dest;

    /* 2. Find reduction arithmetic instruction that uses arr_dest */
    int red_arith_idx = -1;
    int red_op = 0;
    uint32_t acc_in_temp = TEMP_NONE;
    uint32_t acc_out_temp = TEMP_NONE;

    for (int i = arr_load_idx + 1; i <= end; i++) {
        if (skip_mask && skip_mask[i - start]) continue;
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

        if (ins->op == OP_ADD || ins->op == OP_FADD) {
            bool src1_matches = (ins->temp_src1 == arr_dest);
            bool src2_matches = (ins->temp_src2 == arr_dest);

            /* Also check if temp was cast/extended from arr_dest */
            if (!src1_matches && zc_temp_is_user(ins->temp_src1)) {
                int def1 = find_def_backward_in_loop(c, start, i, skip_mask, ins->temp_src1);
                if (def1 >= 0) {
                    Instr *d1 = ir_get(c, def1);
                    if (d1 && (d1->op == OP_SEXT32 || d1->op == OP_ZEXT32 || d1->op == OP_MOV_REG || d1->op == OP_CVTSS2SD)) {
                        uint32_t s = (d1->temp_src1 != TEMP_NONE) ? d1->temp_src1 : d1->temp_dest;
                        if (s == arr_dest) src1_matches = true;
                    }
                }
            }
            if (!src2_matches && zc_temp_is_user(ins->temp_src2)) {
                int def2 = find_def_backward_in_loop(c, start, i, skip_mask, ins->temp_src2);
                if (def2 >= 0) {
                    Instr *d2 = ir_get(c, def2);
                    if (d2 && (d2->op == OP_SEXT32 || d2->op == OP_ZEXT32 || d2->op == OP_MOV_REG || d2->op == OP_CVTSS2SD)) {
                        uint32_t s = (d2->temp_src1 != TEMP_NONE) ? d2->temp_src1 : d2->temp_dest;
                        if (s == arr_dest) src2_matches = true;
                    }
                }
            }

            if (src1_matches || src2_matches) {
                red_arith_idx = i;
                red_op = ins->op;
                acc_in_temp = src1_matches ? ins->temp_src2 : ins->temp_src1;
                acc_out_temp = ins->temp_dest;
                if (ins->op == OP_FADD) is_fp = true;
                break;
            }
        }
    }

    if (red_arith_idx < 0) return false;

    /* 3. Find accumulator store and load */
    int red_store_idx = -1;
    int32_t red_slot = 0x7fffffff;
    int red_temp = -1;

    /* Find store that writes acc_out_temp */
    for (int i = red_arith_idx + 1; i <= end; i++) {
        if (skip_mask && skip_mask[i - start]) continue;
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

        if (ins->op == OP_MOV_MEM && ins->imm < 0 && ins->imm != counter_slot) {
            if (ins->temp_src1 == acc_out_temp) {
                red_store_idx = i;
                red_slot = ins->imm;
                break;
            }
            int def = find_def_backward_in_loop(c, start, i, skip_mask, ins->temp_src1);
            if (def == red_arith_idx) {
                red_store_idx = i;
                red_slot = ins->imm;
                break;
            }
        } else if (ins->op == OP_VSTORESS || ins->op == OP_VSTORESD || ins->op == OP_FSTORE) {
            uint32_t val = (ins->op == OP_FSTORE) ? ins->temp_src1 : ins->temp_src2;
            if (val == acc_out_temp) {
                uint32_t addr = ins->temp_src1;
                int def_addr = find_def_backward_in_loop(c, 0, i, skip_mask, addr);
                if (def_addr >= 0) {
                    Instr *da = ir_get(c, def_addr);
                    if (da && da->op == OP_LEA && da->imm < 0 && da->imm != counter_slot) {
                        red_store_idx = i;
                        red_slot = da->imm;
                        break;
                    }
                } else if (ins->imm < 0 && ins->imm != counter_slot) {
                    red_store_idx = i;
                    red_slot = (int32_t)ins->imm64;
                    break;
                }
            }
        }
    }

    int red_load_idx = -1;
    if (red_slot != 0x7fffffff) {
        /* Find accumulator load from red_slot */
        for (int i = start; i <= end; i++) {
            if (skip_mask && skip_mask[i - start]) continue;
            Instr *ins = ir_get(c, i);
            if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

            if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE && ins->imm == red_slot) {
                red_load_idx = i;
                break;
            } else if (ins->op == OP_VLOADSS || ins->op == OP_VLOADSD || ins->op == OP_FLOAD) {
                uint32_t addr = ins->temp_src1;
                int def_addr = find_def_backward_in_loop(c, 0, i, skip_mask, addr);
                if (def_addr >= 0) {
                    Instr *da = ir_get(c, def_addr);
                    if (da && da->op == OP_LEA && da->imm == red_slot) {
                        red_load_idx = i;
                        break;
                    }
                } else if (ins->imm == red_slot || ins->imm64 == red_slot) {
                    red_load_idx = i;
                    break;
                }
            }
        }
    } else {
        /* Register accumulator */
        uint32_t true_acc = acc_in_temp;
        int def_in = find_def_backward_in_loop(c, start, red_arith_idx, skip_mask, acc_in_temp);
        int store_out = -1;
        if (def_in >= 0) {
            Instr *din = ir_get(c, def_in);
            if (din && din->op == OP_MOV_REG && zc_temp_is_user(din->temp_src1)) {
                for (int j = red_arith_idx + 1; j <= end; j++) {
                    if (skip_mask && skip_mask[j - start]) continue;
                    Instr *dj = ir_get(c, j);
                    if (dj && dj->op == OP_MOV_REG && dj->temp_dest == din->temp_src1 && dj->temp_src1 == acc_out_temp) {
                        true_acc = din->temp_src1;
                        store_out = j;
                        break;
                    }
                }
            }
        }
        if (zc_temp_is_user(true_acc) && zc_temp_is_user(acc_out_temp)) {
            red_temp = (int)true_acc;
            if (store_out >= 0 && def_in >= 0) {
                red_load_idx = def_in;
                red_store_idx = store_out;
            }
        } else {
            return false;
        }
    }

    if (red_slot == 0x7fffffff && red_temp < 0) return false;

    *out_arr_load_idx = arr_load_idx;
    *out_red_load_idx = red_load_idx;
    *out_red_store_idx = red_store_idx;
    *out_red_arith_idx = red_arith_idx;
    *out_red_slot = red_slot;
    *out_red_temp = red_temp;
    *out_red_op = red_op;
    *out_is_fp = is_fp;
    *out_is_double = is_double;

    return true;
}

typedef struct {
    int cmp_idx;        /* OP_CMP */
    int condset_idx;    /* OP_COND_SET */
    int je_idx;         /* OP_JE */
    int then_start;     /* start instruction of then arm */
    int then_end;       /* end instruction of then arm */
    int jmp_idx;        /* OP_JMP (or -1 if single arm) */
    int else_label_idx; /* OP_LABEL (or -1) */
    int else_start;     /* start instruction of else arm (or -1) */
    int else_end;       /* end instruction of else arm (or -1) */
    int end_label_idx;  /* OP_LABEL */
    int res_temp;       /* destination temp receiving the selected value */
    int then_val;       /* temp holding then-value */
    int else_val;       /* temp holding else-value */
    int cond;           /* condition code (CC_GT, CC_LT, etc.) */
    bool is_fp;         /* true if floating point comparison */
    int esz;            /* 4 or 8 */
    int vec_mask;       /* allocated vector mask temp */
    int vec_res;        /* allocated vector result temp */
    int vec_then;       /* allocated or mapped vector then temp */
    int vec_else;       /* allocated or mapped vector else temp */
} LoopCondDiamond;

static int detect_loop_diamonds(ZeroCCCompiler *c, int start, int end, int test_idx,
                                const uint8_t *skip_mask, LoopCondDiamond *diamonds, int max_diamonds) {
    int count = 0;
    for (int i = start + 1; i < end; i++) {
        if (i == test_idx || (skip_mask && skip_mask[i - start])) continue;
        Instr *je = ir_get(c, i);
        if (!je || je->op != OP_JE) continue;
        uint32_t cond_t = je->temp_src1;
        if (!zc_temp_is_user(cond_t)) continue;

        int condset_idx = -1;
        for (int k = i - 1; k >= start; k--) {
            if (skip_mask && skip_mask[k - start]) continue;
            Instr *ins = ir_get(c, k);
            if (ins && ins->op == OP_COND_SET && ins->temp_dest == cond_t) {
                condset_idx = k;
                break;
            }
        }
        if (condset_idx < 0) continue;

        int cmp_idx = -1;
        for (int k = condset_idx - 1; k >= start; k--) {
            if (skip_mask && skip_mask[k - start]) continue;
            Instr *ins = ir_get(c, k);
            if (ins && (ins->op == OP_CMP || ins->op == OP_VCMPSS || ins->op == OP_VCMPSD)) {
                cmp_idx = k;
                break;
            }
        }
        if (cmp_idx < 0) continue;

        int else_lid = (int)je->label_idx;
        int target_pos = -1;
        for (int k = start; k <= end; k++) {
            Instr *ins = ir_get(c, k);
            if (ins && ins->op == OP_LABEL && (int)ins->label_idx == else_lid) {
                target_pos = k;
                break;
            }
        }
        if (target_pos <= i || target_pos >= end) continue;

        Instr *before_target = ir_get(c, target_pos - 1);
        if (before_target && before_target->op == OP_JMP) {
            int end_lid = (int)before_target->label_idx;
            int end_pos = -1;
            for (int k = target_pos + 1; k <= end; k++) {
                Instr *ins = ir_get(c, k);
                if (ins && ins->op == OP_LABEL && (int)ins->label_idx == end_lid) {
                    end_pos = k;
                    break;
                }
            }
            if (end_pos <= target_pos || end_pos > end) continue;

            int then_last = target_pos - 2;
            int then_val = -1;
            int res_t = -1;
            for (int k = i + 1; k <= then_last; k++) {
                if (skip_mask && skip_mask[k - start]) continue;
                Instr *ins = ir_get(c, k);
                if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
                if (ins->op == OP_MOV_REG || ins->op == OP_FMOV) {
                    res_t = (int)ins->temp_dest;
                    then_val = (int)ins->temp_src1;
                }
            }
            if (res_t < 0 || then_val < 0) continue;

            int else_last = end_pos - 1;
            int else_val = -1;
            for (int k = target_pos + 1; k <= else_last; k++) {
                if (skip_mask && skip_mask[k - start]) continue;
                Instr *ins = ir_get(c, k);
                if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
                if ((ins->op == OP_MOV_REG || ins->op == OP_FMOV) && (int)ins->temp_dest == res_t) {
                    else_val = (int)ins->temp_src1;
                }
            }
            if (else_val < 0) continue;

            Instr *cmp_ins = ir_get(c, cmp_idx);
            Instr *cs_ins = ir_get(c, condset_idx);
            bool is_fp = (cmp_ins->op == OP_VCMPSS || cmp_ins->op == OP_VCMPSD ||
                          (cmp_ins->flags_out == 0xFE) ||
                          ((zc_temp_tag(c, cmp_ins->temp_src1) & 0x0F) == ZC_TAGCLS_FLOAT));

            if (count < max_diamonds) {
                LoopCondDiamond *d = &diamonds[count++];
                d->cmp_idx = cmp_idx;
                d->condset_idx = condset_idx;
                d->je_idx = i;
                d->then_start = i + 1;
                d->then_end = then_last;
                d->jmp_idx = target_pos - 1;
                d->else_label_idx = target_pos;
                d->else_start = target_pos + 1;
                d->else_end = else_last;
                d->end_label_idx = end_pos;
                d->res_temp = res_t;
                d->then_val = then_val;
                d->else_val = else_val;
                d->cond = (int)cs_ins->cond;
                d->is_fp = is_fp;
                d->esz = is_fp ? (cmp_ins->op == OP_VCMPSD ? 8 : 4) : 4;
                d->vec_mask = -1;
                d->vec_res = -1;
                d->vec_then = -1;
                d->vec_else = -1;
            }
            i = end_pos;
        } else {
            /* Single-arm if without else */
            int end_pos = target_pos;
            int then_last = end_pos - 1;
            int then_val = -1;
            int res_t = -1;
            for (int k = i + 1; k <= then_last; k++) {
                if (skip_mask && skip_mask[k - start]) continue;
                Instr *ins = ir_get(c, k);
                if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
                if (ins->op == OP_MOV_REG || ins->op == OP_FMOV) {
                    res_t = (int)ins->temp_dest;
                    then_val = (int)ins->temp_src1;
                }
            }
            if (res_t < 0 || then_val < 0) continue;

            Instr *cmp_ins = ir_get(c, cmp_idx);
            Instr *cs_ins = ir_get(c, condset_idx);
            bool is_fp = (cmp_ins->op == OP_VCMPSS || cmp_ins->op == OP_VCMPSD ||
                          (cmp_ins->flags_out == 0xFE) ||
                          ((zc_temp_tag(c, cmp_ins->temp_src1) & 0x0F) == ZC_TAGCLS_FLOAT));

            if (count < max_diamonds) {
                LoopCondDiamond *d = &diamonds[count++];
                d->cmp_idx = cmp_idx;
                d->condset_idx = condset_idx;
                d->je_idx = i;
                d->then_start = i + 1;
                d->then_end = then_last;
                d->jmp_idx = -1;
                d->else_label_idx = -1;
                d->else_start = -1;
                d->else_end = -1;
                d->end_label_idx = end_pos;
                d->res_temp = res_t;
                d->then_val = then_val;
                d->else_val = res_t;
                d->cond = (int)cs_ins->cond;
                d->is_fp = is_fp;
                d->esz = is_fp ? (cmp_ins->op == OP_VCMPSD ? 8 : 4) : 4;
                d->vec_mask = -1;
                d->vec_res = -1;
                d->vec_then = -1;
                d->vec_else = -1;
            }
            i = end_pos;
        }
    }
    return count;
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
    vinfo->is_scalable = false;
    vinfo->is_rvv = false;
    vinfo->is_sve = false;
    vinfo->bound_temp = TEMP_NONE;
    vinfo->bound_slot = 0x7fffffff;
    vinfo->counter_slot = 0x7fffffff;
    vinfo->has_strided_access = false;

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
    bool has_int8_mem = false;
    bool has_int16_mem = false;
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
        if (!instr || instr->op == OP_DEAD) continue;
        if (instr->op == OP_NOP) {
            if (instr->imm == VEC_PEEL_MARKER) {
                has_call = true;
                break;
            }
            continue;
        }

        if (instr->op == OP_CALL || instr->op == OP_CALL_INDIRECT ||
            instr->op == OP_RET || instr->op == OP_SYSCALL ||
            instr->op == OP_CLONE || instr->op == OP_ASM ||
            instr->op == OP_SETJMP || instr->op == OP_LONGJMP ||
            instr->op == OP_OVF_TRAP || instr->op == OP_TRAP ||
            instr->op == OP_ASAN_CHECK) {
            has_call = true;
            continue;
        }

        if (instr->op == OP_LOAD_BYTE) {
            load_count++;
            if (base_temp < 0) base_temp = (int)instr->temp_src1;
            has_int8_mem = true;
        } else if (instr->op == OP_LOAD_16) {
            load_count++;
            if (base_temp < 0) base_temp = (int)instr->temp_src1;
            has_int16_mem = true;
        } else if (instr->op == OP_LOAD_32 || instr->op == OP_FLOAD ||
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
        if (instr->op == OP_STORE_BYTE) {
            store_count++;
            has_int8_mem = true;
        } else if (instr->op == OP_STORE_16) {
            store_count++;
            has_int16_mem = true;
        } else if (instr->op == OP_STORE_32 || instr->op == OP_FSTORE ||
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

    bool is_pure_copy = (store_count >= 1 && load_count >= 1);
    int mem_kind_count = (has_int8_mem ? 1 : 0) + (has_int16_mem ? 1 : 0) + (has_int32_mem ? 1 : 0) + (has_f32_mem ? 1 : 0) + (has_f64_mem ? 1 : 0);
    if (has_call || has_int_divmod || (mem_kind_count > 1) ||
        load_count < 1 || (arith_count < 1 && !is_pure_copy)) {
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

    LoopCondDiamond diamonds[16];
    int diamond_count = detect_loop_diamonds(c, start, end, -1, skip_mask, diamonds, 16);

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
            bool is_diamond_branch = false;
            for (int d = 0; d < diamond_count; d++) {
                if (last == diamonds[d].je_idx || (diamonds[d].jmp_idx >= 0 && last == diamonds[d].jmp_idx)) {
                    is_diamond_branch = true;
                    break;
                }
            }
            if (is_diamond_branch) continue;

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
            bool is_diamond_branch = false;
            for (int d = 0; d < diamond_count; d++) {
                if (idx == diamonds[d].je_idx) {
                    is_diamond_branch = true;
                    break;
                }
            }
            if (!is_diamond_branch) {
                has_mid_body_cond = true;
            }
        }
    }

    if (!has_mid_body_cond && test_idx > start) {
        vinfo->has_cond_select = (diamond_count > 0);
        vinfo->cond_diamond_count = diamond_count;
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
            int def_o1 = -1, def_o2 = -1;
            bool ok1 = (o1 != TEMP_NONE && resolve_const_at(c, cmp_idx, o1, &cval1, &def_o1));
            bool ok2 = (o2 != TEMP_NONE && resolve_const_at(c, cmp_idx, o2, &cval2, &def_o2));
            bool c1 = ok1 && (def_o1 >= start || !temp_redefined_in_span(c, start, end, skip_mask, o1));
            bool c2 = ok2 && (def_o2 >= start || !temp_redefined_in_span(c, start, end, skip_mask, o2));
            bool has_sve = state->comp && state->comp->has_sve && (state->target_isa == ISA_AARCH64);
            bool has_rvv = state->comp && state->comp->has_rvv && (state->target_isa == ISA_RISCV64 || state->target_isa == ISA_RISCV32);
            bool target_supports_scalable = has_rvv || has_sve;
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
                int64_t init = 0;
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
                                else { init = init_v; trip -= init_v; }
                            }
                            break;
                        }
                    }
                }
                bool is_red = false;
                int red_arr_load = -1, red_ld = -1, red_st = -1, red_ar = -1;
                int32_t r_slot = 0x7fffffff;
                int r_temp = -1, r_op = 0;
                bool r_fp = false, r_dbl = false;
                if (detect_loop_reduction(c, start, end, skip_mask, loop_var, counter_slot,
                                          &red_arr_load, &red_ld, &red_st, &red_ar,
                                          &r_slot, &r_temp, &r_op, &r_fp, &r_dbl)) {
                    is_red = true;
                    vinfo->is_reduction = true;
                    vinfo->reduction_op = r_op;
                    vinfo->reduction_slot = r_slot;
                    vinfo->reduction_temp = r_temp;
                    vinfo->red_load_idx = red_ld;
                    vinfo->red_store_idx = red_st;
                    vinfo->red_arith_idx = red_ar;
                    vinfo->is_reduction_fp = r_fp;
                    has_double = r_dbl;
                    has_fp = r_fp;
                }

                if (target_supports_scalable && trip > 0) {
                    bound_ok = true;
                    vinfo->is_scalable = true;
                    vinfo->is_rvv = has_rvv;
                    vinfo->is_sve = has_sve;
                    vinfo->needs_peel = false;
                    vinfo->v_bound = bound;
                    vinfo->test_instr_idx = test_idx;
                    vinfo->cmp_instr_idx = cmp_idx;
                    vinfo->bound_is_src2 = (!c1 && c2);
                    vinfo->bound_temp = TEMP_NONE;
                } else {
                    bool has_avx = state->comp && state->comp->has_avx && (state->target_isa == ISA_X86_64);
                    bool has_avx2 = state->comp && state->comp->has_avx2 && (state->target_isa == ISA_X86_64);
                    bool has_avx512 = state->comp && state->comp->has_avx512f && (state->target_isa == ISA_X86_64);
                    int elem_size_pre = 4;
                    if (has_double) elem_size_pre = 8;
                    else if (has_int8_mem) elem_size_pre = 1;
                    else if (has_int16_mem) elem_size_pre = 2;
                    int width = is_red ? (r_dbl ? 2 : 4) :
                                (has_double ? (has_avx512 ? 8 : (has_avx ? 4 : 2)) :
                                (elem_size_pre == 1 ? (has_avx2 ? 32 : 16) :
                                (elem_size_pre == 2 ? (has_avx2 ? 16 : 8) : state->vector_width)));
                    int64_t v_trip = trip;
                    bool needs_peel = false;
                    if (trip >= width) {
                        if ((trip % width) == 0) {
                            bound_ok = true;
                            v_trip = trip;
                        } else {
                            v_trip = trip - (trip % width);
                            if (v_trip >= width && test_idx < end && ir_get(c, end) && ir_get(c, end)->op == OP_JMP) {
                                bound_ok = true;
                                needs_peel = true;
                            }
                        }
                    }
                    if (bound_ok) {
                        int64_t v_bound = 0;
                        bool bound_is_src2 = (!c1 && c2);
                        if (bound_is_src2) {
                            if (cs->cond == CC_LT) v_bound = init + v_trip;
                            else if (cs->cond == CC_LE) v_bound = init + v_trip - 1;
                        } else {
                            if (cs->cond == CC_GT) v_bound = init + v_trip;
                            else if (cs->cond == CC_GE) v_bound = init + v_trip - 1;
                        }
                        vinfo->needs_peel = needs_peel;
                        vinfo->v_bound = v_bound;
                        vinfo->test_instr_idx = test_idx;
                        vinfo->cmp_instr_idx = cmp_idx;
                        vinfo->bound_is_src2 = bound_is_src2;
                    }
                }
            } else if (!c1 && !c2 && target_supports_scalable) {
                int32_t slot1 = 0x7fffffff, slot2 = 0x7fffffff;
                bool o1_inv = is_temp_or_slot_invariant_bound(c, start, end, cmp_idx, skip_mask, o1, &slot1);
                bool o2_inv = is_temp_or_slot_invariant_bound(c, start, end, cmp_idx, skip_mask, o2, &slot2);
                if (!o1_inv && o2_inv && (cs->cond == CC_LT || cs->cond == CC_LE)) {
                    uint32_t ctr_t = o1;
                    uint32_t bound_t = o2;
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
                    if (loop_var >= 0) {
                        bound_ok = true;
                        vinfo->is_scalable = true;
                        vinfo->is_rvv = has_rvv;
                        vinfo->is_sve = has_sve;
                        vinfo->needs_peel = false;
                        vinfo->bound_temp = bound_t;
                        vinfo->bound_slot = slot2;
                        vinfo->test_instr_idx = test_idx;
                        vinfo->cmp_instr_idx = cmp_idx;
                        vinfo->bound_is_src2 = true;
                    }
                } else if (o1_inv && !o2_inv && (cs->cond == CC_GT || cs->cond == CC_GE)) {
                    uint32_t ctr_t = o2;
                    uint32_t bound_t = o1;
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
                    if (loop_var >= 0) {
                        bound_ok = true;
                        vinfo->is_scalable = true;
                        vinfo->is_rvv = has_rvv;
                        vinfo->is_sve = has_sve;
                        vinfo->needs_peel = false;
                        vinfo->bound_temp = bound_t;
                        vinfo->bound_slot = slot1;
                        vinfo->test_instr_idx = test_idx;
                        vinfo->cmp_instr_idx = cmp_idx;
                        vinfo->bound_is_src2 = false;
                    }
                }
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

    if (!vinfo->is_reduction && (store_count < 1 || load_count < 2)) {
        if (skip_mask != skip_buf) free(skip_mask);
        return;
    }

    int elem_size = has_double ? 8 : (has_int8_mem ? 1 : (has_int16_mem ? 2 : 4));
    bool strides_ok = true;
    bool chains_ok = true;
    bool all_aligned = true;
    bool any_aligned_checked = false;

    for (int i = start; i <= end && (strides_ok || chains_ok); i++) {
        if (skip_mask[i - start]) continue;
        Instr *a = ir_get(c, i);
        if (!a) continue;
        if (vinfo->is_reduction && (i == vinfo->red_load_idx || i == vinfo->red_store_idx)) {
            continue;
        }
        bool is_load = (a->op == OP_LOAD_BYTE || a->op == OP_LOAD_16 ||
                        a->op == OP_LOAD_32 || a->op == OP_FLOAD ||
                        a->op == OP_VLOADSS || a->op == OP_VLOADSD);
        bool is_store = (a->op == OP_STORE_BYTE || a->op == OP_STORE_16 || a->op == OP_STORE_32);
        bool is_storef = (a->op == OP_FSTORE || a->op == OP_VSTORESS || a->op == OP_VSTORESD);
        if (!is_load && !is_store && !is_storef) continue;

        uint32_t addr = is_store ? a->temp_src2 : a->temp_src1;
        uint32_t val  = is_store ? a->temp_src1 : a->temp_src2;
        int scale = 0;
        int32_t base_off = 0;
        bool have_base = false;
        int cls = classify_addr_temp(c, start, end, i, skip_mask, addr,
                                     loop_var, counter_slot, 0, &scale,
                                     &base_off, &have_base);
        if (cls == 2) {
            if (scale <= 0 || (scale % elem_size) != 0) {
                strides_ok = false;
                continue;
            }
            vinfo->has_indexed_access = true;
        } else if (cls != 1 || scale <= 0 || (scale % elem_size) != 0) {
            strides_ok = false;
            continue;
        }
        if (cls == 1 && scale > elem_size) {
            vinfo->has_strided_access = true;
        }
        if (cls == 1 && have_base) {
            any_aligned_checked = true;
            if (((-base_off) & 15) != 0) all_aligned = false;
        } else {
            all_aligned = false;
        }
        if (!vinfo->is_reduction && (is_store || is_storef) &&
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
    vinfo->is_int32 = (elem_size == 4 && !has_fp);
    vinfo->is_int16 = (elem_size == 2);
    vinfo->is_int8 = (elem_size == 1);
    vinfo->loop_var_temp = loop_var;
    vinfo->array_base_temp = base_temp;
    vinfo->inc_instr_idx = inc_idx;
    vinfo->inc_use_src2 = inc_use_src2;
    vinfo->counter_slot = counter_slot;
    vinfo->stride_bytes = elem_size;
    vinfo->is_aligned = any_aligned_checked && all_aligned;
    if (vinfo->is_scalable && !vinfo->has_strided_access) {
        vinfo->vector_width = (elem_size == 8) ? 2 : (elem_size == 2) ? 8 : (elem_size == 1) ? 16 : 4;
        vinfo->elem_size = elem_size;
        vinfo->array_stride = elem_size;
    } else {
        bool has_avx = state->comp && state->comp->has_avx && (state->target_isa == ISA_X86_64);
        bool has_avx2 = state->comp && state->comp->has_avx2 && (state->target_isa == ISA_X86_64);
        bool has_avx512 = state->comp && state->comp->has_avx512f && (state->target_isa == ISA_X86_64);
        if (vinfo->is_reduction || vinfo->has_strided_access || vinfo->has_indexed_access) {
            vinfo->vector_width = (elem_size == 8) ? 2 : (elem_size == 2) ? 8 : (elem_size == 1) ? 16 : 4;
            vinfo->elem_size = elem_size;
            vinfo->array_stride = elem_size;
        } else if (has_double) {
            vinfo->vector_width = has_avx512 ? 8 : (has_avx ? 4 : 2);
            vinfo->elem_size = 8;
            vinfo->array_stride = 8;
        } else if (elem_size == 1) {
            vinfo->vector_width = has_avx2 ? 32 : 16;
            vinfo->elem_size = 1;
            vinfo->array_stride = 1;
        } else if (elem_size == 2) {
            vinfo->vector_width = has_avx2 ? 16 : 8;
            vinfo->elem_size = 2;
            vinfo->array_stride = 2;
        } else {
            vinfo->vector_width = state->vector_width;
            vinfo->elem_size = 4;
            vinfo->array_stride = 4;
        }
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

static int get_rvv_op(int scalar_op) {
    switch (scalar_op) {
        case OP_ADD:  case OP_FADD: return OP_RVV_VADD;
        case OP_SUB:  case OP_FSUB: return OP_RVV_VSUB;
        case OP_MUL:  case OP_FMUL: return OP_RVV_VMUL;
        case OP_DIV:  case OP_FDIV: return OP_RVV_VDIV;
        case OP_AND:  return OP_RVV_VAND;
        case OP_OR:   return OP_RVV_VOR;
        case OP_XOR:  return OP_RVV_VXOR;
        default:      return scalar_op;
    }
}

static int get_sve_op(int scalar_op) {
    switch (scalar_op) {
        case OP_ADD:  case OP_FADD: return OP_SVE_ADD;
        case OP_SUB:  case OP_FSUB: return OP_SVE_SUB;
        case OP_MUL:  case OP_FMUL: return OP_SVE_MUL;
        case OP_DIV:  case OP_FDIV: return OP_SVE_DIV;
        case OP_AND:  return OP_SVE_AND;
        case OP_OR:   return OP_SVE_ORR;
        case OP_XOR:  return OP_SVE_EOR;
        default:      return scalar_op;
    }
}

/**
 * Get vector operation for scalar operation
 */
static int get_vec_op(int scalar_op, bool is_double, bool is_int32, bool is_int16, bool is_int8, bool is_256, bool is_512) {
    if (is_512) {
        if (is_double) {
            switch (scalar_op) {
                case OP_ADD:
                case OP_FADD: return OP_V512_ADDPD;
                case OP_SUB:
                case OP_FSUB: return OP_V512_SUBPD;
                case OP_MUL:
                case OP_FMUL: return OP_V512_MULPD;
                case OP_DIV:
                case OP_FDIV: return OP_V512_DIVPD;
                case OP_AND:  return OP_V512_ANDPS;
                case OP_OR:   return OP_V512_VORPS;
                case OP_XOR:  return OP_V512_VXORPS;
                default:      return scalar_op;
            }
        } else if (is_int32) {
            switch (scalar_op) {
                case OP_ADD:  return OP_V512_PADDD;
                case OP_SUB:  return OP_V512_PSUBD;
                case OP_MUL:  return OP_V512_PMULLD;
                case OP_AND:  return OP_V512_PANDD;
                case OP_OR:   return OP_V512_PORD;
                case OP_XOR:  return OP_V512_PXORD;
                default:      return scalar_op;
            }
        } else {
            switch (scalar_op) {
                case OP_ADD:
                case OP_FADD: return OP_V512_ADDPS;
                case OP_SUB:
                case OP_FSUB: return OP_V512_SUBPS;
                case OP_MUL:
                case OP_FMUL: return OP_V512_MULPS;
                case OP_DIV:
                case OP_FDIV: return OP_V512_DIVPS;
                case OP_AND:  return OP_V512_ANDPS;
                case OP_OR:   return OP_V512_VORPS;
                case OP_XOR:  return OP_V512_VXORPS;
                default:      return scalar_op;
            }
        }
    }
    if (is_256) {
        if (is_double) {
            switch (scalar_op) {
                case OP_ADD:
                case OP_FADD: return OP_V256_ADDPD;
                case OP_SUB:
                case OP_FSUB: return OP_V256_SUBPD;
                case OP_MUL:
                case OP_FMUL: return OP_V256_MULPD;
                case OP_DIV:
                case OP_FDIV: return OP_V256_DIVPD;
                case OP_AND:  return OP_V256_ANDPS;
                case OP_OR:   return OP_VORPS;
                case OP_XOR:  return OP_VXORPS;
                default:      return scalar_op;
            }
        } else if (is_int32) {
            switch (scalar_op) {
                case OP_ADD:  return OP_V256_PADDD;
                case OP_SUB:  return OP_V256_PSUBD;
                case OP_MUL:  return OP_V256_PMULLD;
                case OP_AND:  return OP_V256_PAND;
                case OP_OR:   return OP_V256_POR;
                case OP_XOR:  return OP_V256_PXOR;
                default:      return scalar_op;
            }
        } else if (is_int16) {
            switch (scalar_op) {
                case OP_ADD:  return OP_V256_PADDW;
                case OP_SUB:  return OP_V256_PSUBW;
                case OP_MUL:  return OP_V256_PMULLW;
                case OP_AND:  return OP_V256_PAND;
                case OP_OR:   return OP_V256_POR;
                case OP_XOR:  return OP_V256_PXOR;
                default:      return scalar_op;
            }
        } else if (is_int8) {
            switch (scalar_op) {
                case OP_ADD:  return OP_V256_PADDB;
                case OP_SUB:  return OP_V256_PSUBB;
                case OP_AND:  return OP_V256_PAND;
                case OP_OR:   return OP_V256_POR;
                case OP_XOR:  return OP_V256_PXOR;
                default:      return scalar_op;
            }
        } else {
            switch (scalar_op) {
                case OP_ADD:
                case OP_FADD: return OP_V256_ADDPS;
                case OP_SUB:
                case OP_FSUB: return OP_V256_SUBPS;
                case OP_MUL:
                case OP_FMUL: return OP_V256_MULPS;
                case OP_DIV:
                case OP_FDIV: return OP_V256_DIVPS;
                case OP_AND:  return OP_V256_ANDPS;
                case OP_OR:   return OP_VORPS;
                case OP_XOR:  return OP_VXORPS;
                default:      return scalar_op;
            }
        }
    }
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
            case OP_AND:  return OP_VANDPS;
            case OP_OR:   return OP_VORPS;
            case OP_XOR:  return OP_VXORPS;
            default:      return scalar_op;
        }
    } else if (is_int8) {
        switch (scalar_op) {
            case OP_ADD:  return OP_VPADDB;
            case OP_SUB:  return OP_VPSUBB;
            case OP_AND:  return OP_VANDPS;
            case OP_OR:   return OP_VORPS;
            case OP_XOR:  return OP_VXORPS;
            default:      return scalar_op;
        }
    } else if (is_int16) {
        switch (scalar_op) {
            case OP_ADD:  return OP_VPADDW;
            case OP_SUB:  return OP_VPSUBW;
            case OP_MUL:  return OP_VPMULLW;
            case OP_AND:  return OP_VANDPS;
            case OP_OR:   return OP_VORPS;
            case OP_XOR:  return OP_VXORPS;
            default:      return scalar_op;
        }
    } else if (is_int32) {
        switch (scalar_op) {
            case OP_ADD:  return OP_VPADDD;
            case OP_SUB:  return OP_VPSUBD;
            case OP_MUL:  return OP_VPMULLD;
            case OP_AND:  return OP_VANDPS;
            case OP_OR:   return OP_VORPS;
            case OP_XOR:  return OP_VXORPS;
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
            case OP_AND:  return OP_VANDPS;
            case OP_OR:   return OP_VORPS;
            case OP_XOR:  return OP_VXORPS;
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
    int total_bytes = vec_width * vinfo->elem_size;
    bool is_512 = !vinfo->is_scalable && !vinfo->is_int8 && !vinfo->is_int16 && (total_bytes >= 64);
    bool is_256 = !vinfo->is_scalable && !is_512 && (total_bytes >= 32);
    if (vinfo->is_scalable) {
        if (vinfo->is_rvv && c) c->rvv_dynamic_vl = true;
    } else {
        if (is_512 && c) { c->used_avx512 = true; c->used_avx = true; }
        else if (is_256 && c) c->used_avx = true;
    }
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

    int red_exit_label = -1;
    int orig_exit_label = -1;
    int rem_head_label = -1;
    if (vinfo->is_reduction) {
        red_exit_label = (int)c->ir_next_label++;
        Instr *test_ins = ir_get(c, vinfo->test_instr_idx);
        if (test_ins) orig_exit_label = (int)test_ins->label_idx;
    }

    /* 1.5 If remainder peeling is needed, clone scalar loop after end_idx and retarget vector loop */
    if (!vinfo->is_scalable && vinfo->needs_peel && vinfo->test_instr_idx >= start_idx && vinfo->test_instr_idx <= end_idx &&
        vinfo->cmp_instr_idx >= start_idx && vinfo->cmp_instr_idx <= end_idx) {
        int orig_head_label = (int)ir_get(c, start_idx)->label_idx;
        rem_head_label = (int)c->ir_next_label++;

        int rem_label_map[VEC_LABEL_MAP_CAP];
        for (int i = 0; i < VEC_LABEL_MAP_CAP; i++) rem_label_map[i] = -1;
        if (orig_head_label < VEC_LABEL_MAP_CAP) rem_label_map[orig_head_label] = rem_head_label;

        for (int k = start_idx + 1; k < end_idx; k++) {
            const Instr *ins = ir_get(c, k);
            if (ins && ins->op == OP_LABEL && ins->label_idx < VEC_LABEL_MAP_CAP) {
                if (rem_label_map[ins->label_idx] < 0) {
                    rem_label_map[ins->label_idx] = (int)c->ir_next_label++;
                }
            }
        }

        size_t orig_len = (size_t)(end_idx - start_idx + 1);
        Instr *rem_instrs = (Instr *)malloc(orig_len * sizeof(Instr));
        if (!rem_instrs) return false;

        for (size_t k = 0; k < orig_len; k++) {
            Instr ins = *ir_get(c, start_idx + (int)k);
            if (ins.op == OP_LABEL) {
                if (ins.label_idx < VEC_LABEL_MAP_CAP && rem_label_map[ins.label_idx] >= 0) {
                    ins.label_idx = (uint16_t)rem_label_map[ins.label_idx];
                }
            } else if (ins.op == OP_JMP || ins.op == OP_JE || ins.op == OP_JNE ||
                       ins.op == OP_JL || ins.op == OP_JLE || ins.op == OP_JG ||
                       ins.op == OP_JCOND) {
                if (ins.label_idx < VEC_LABEL_MAP_CAP && rem_label_map[ins.label_idx] >= 0) {
                    ins.label_idx = (uint16_t)rem_label_map[ins.label_idx];
                }
            }
            rem_instrs[k] = ins;
        }

        /* Insert remainder loop instructions right after end_idx */
        for (size_t k = 0; k < orig_len; k++) {
            ir_insert_at(c, (size_t)(end_idx + 1 + (int)k), rem_instrs[k]);
        }
        free(rem_instrs);

        /* Insert peel marker right after remainder loop header to avoid re-peeling */
        Instr marker = {
            .op = OP_NOP,
            .temp_dest = TEMP_NONE,
            .temp_src1 = TEMP_NONE,
            .temp_src2 = TEMP_NONE,
            .cond = 0,
            .flags_out = 0xFF,
            .flags_in = 0xFF,
            .imm = VEC_PEEL_MARKER,
            .imm64 = VEC_PEEL_MARKER,
            .label_idx = 0
        };
        ir_insert_at(c, (size_t)(end_idx + 2), marker);

        /* Vector loop exit test jumps to remainder loop header (or red_exit_label for reductions) */
        Instr *test_ins = ir_get(c, vinfo->test_instr_idx);
        if (test_ins) {
            test_ins->label_idx = (uint16_t)(vinfo->is_reduction ? red_exit_label : rem_head_label);
        }

        /* Insert v_bound definition right before CMP in vector loop */
        int v_bound_temp = zc_alloc_temp(c);
        Instr mov_vbound = {
            .op = OP_MOV_IMM,
            .temp_dest = (uint32_t)v_bound_temp,
            .temp_src1 = TEMP_NONE,
            .temp_src2 = TEMP_NONE,
            .cond = 0,
            .flags_out = 0xFF,
            .flags_in = 0xFF,
            .imm = (int16_t)vinfo->v_bound,
            .imm64 = vinfo->v_bound,
            .label_idx = 0
        };
        ir_insert_at(c, (size_t)vinfo->cmp_instr_idx, mov_vbound);
        end_idx++;
        if (vinfo->test_instr_idx >= vinfo->cmp_instr_idx) vinfo->test_instr_idx++;
        if (vinfo->inc_instr_idx >= vinfo->cmp_instr_idx) vinfo->inc_instr_idx++;
        if (vinfo->red_load_idx >= vinfo->cmp_instr_idx) vinfo->red_load_idx++;
        if (vinfo->red_store_idx >= vinfo->cmp_instr_idx) vinfo->red_store_idx++;
        if (vinfo->red_arith_idx >= vinfo->cmp_instr_idx) vinfo->red_arith_idx++;

        Instr *cmp_ins = ir_get(c, vinfo->cmp_instr_idx + 1);
        if (cmp_ins) {
            if (vinfo->bound_is_src2) cmp_ins->temp_src2 = (uint32_t)v_bound_temp;
            else cmp_ins->temp_src1 = (uint32_t)v_bound_temp;
        }

        c->cfg_cache.valid = false;
    }

    if (vinfo->is_reduction && !vinfo->needs_peel) {
        Instr *test_ins = ir_get(c, vinfo->test_instr_idx);
        if (test_ins) test_ins->label_idx = (uint16_t)red_exit_label;
    }

    int v_acc = -1;
    if (vinfo->is_reduction) {
        v_acc = zc_alloc_temp(c);
        Instr zero_acc = {
            .op = OP_VXORPS,
            .temp_dest = (uint32_t)v_acc,
            .temp_src1 = (uint32_t)v_acc,
            .temp_src2 = (uint32_t)v_acc,
            .cond = 0,
            .flags_out = 0xFF,
            .flags_in = 0xFF,
            .imm = 0,
            .imm64 = 0,
            .label_idx = 0
        };
        ir_insert_at(c, (size_t)start_idx, zero_acc);
        start_idx++;
        end_idx++;
        if (vinfo->inc_instr_idx >= start_idx - 1) vinfo->inc_instr_idx++;
        if (vinfo->test_instr_idx >= start_idx - 1) vinfo->test_instr_idx++;
        if (vinfo->cmp_instr_idx >= start_idx - 1) vinfo->cmp_instr_idx++;
        if (vinfo->red_load_idx >= start_idx - 1) vinfo->red_load_idx++;
        if (vinfo->red_store_idx >= start_idx - 1) vinfo->red_store_idx++;
        if (vinfo->red_arith_idx >= start_idx - 1) vinfo->red_arith_idx++;
    }

    /* 2. Rewrite loop increment */
    if (vinfo->is_scalable) {
        uint32_t bound_t = vinfo->bound_temp;
        int insert_pos = vinfo->test_instr_idx + 1;
        if (bound_t == TEMP_NONE) {
            int bt = zc_alloc_temp(c);
            Instr mov_b = {
                .op = OP_MOV_IMM,
                .temp_dest = (uint32_t)bt,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)vinfo->v_bound,
                .imm64 = vinfo->v_bound,
                .label_idx = 0
            };
            ir_insert_at(c, (size_t)vinfo->cmp_instr_idx, mov_b);
            end_idx++;
            if (vinfo->test_instr_idx >= vinfo->cmp_instr_idx) vinfo->test_instr_idx++;
            if (vinfo->inc_instr_idx >= vinfo->cmp_instr_idx) vinfo->inc_instr_idx++;
            bound_t = (uint32_t)bt;

            Instr *cmp_ins = ir_get(c, vinfo->cmp_instr_idx + 1);
            if (cmp_ins) {
                if (vinfo->bound_is_src2) cmp_ins->temp_src2 = bound_t;
                else cmp_ins->temp_src1 = bound_t;
            }
            insert_pos = vinfo->test_instr_idx + 1;
        } else if (vinfo->bound_slot != 0x7fffffff) {
            int bt = zc_alloc_temp(c);
            Instr mov_b = {
                .op = OP_MOV,
                .temp_dest = (uint32_t)bt,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)vinfo->bound_slot,
                .imm64 = vinfo->bound_slot,
                .label_idx = 0
            };
            ir_insert_at(c, (size_t)insert_pos, mov_b);
            insert_pos++;
            end_idx++;
            if (vinfo->inc_instr_idx >= insert_pos - 1) vinfo->inc_instr_idx++;
            bound_t = (uint32_t)bt;
        }

        int cur_var_t = vinfo->loop_var_temp;
        if (vinfo->counter_slot != 0x7fffffff) {
            int ct = zc_alloc_temp(c);
            Instr mov_c = {
                .op = OP_MOV,
                .temp_dest = (uint32_t)ct,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)vinfo->counter_slot,
                .imm64 = vinfo->counter_slot,
                .label_idx = 0
            };
            ir_insert_at(c, (size_t)insert_pos, mov_c);
            insert_pos++;
            end_idx++;
            if (vinfo->inc_instr_idx >= insert_pos - 1) vinfo->inc_instr_idx++;
            cur_var_t = ct;
        }

        int esz = vinfo->is_double ? 3 : 2;
        int step_t = -1;

        if (vinfo->is_rvv) {
            int rem_t = zc_alloc_temp(c);
            int vl_t = zc_alloc_temp(c);
            Instr sub_rem = {
                .op = OP_SUB,
                .temp_dest = (uint32_t)rem_t,
                .temp_src1 = bound_t,
                .temp_src2 = (uint32_t)cur_var_t,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = 0,
                .imm64 = 0,
                .label_idx = 0
            };
            Instr setvl = {
                .op = OP_RVV_VSETVLI,
                .temp_dest = (uint32_t)vl_t,
                .temp_src1 = (uint32_t)rem_t,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)esz,
                .imm64 = 0,
                .label_idx = 0
            };
            ir_insert_at(c, (size_t)insert_pos, sub_rem);
            ir_insert_at(c, (size_t)(insert_pos + 1), setvl);
            end_idx += 2;
            if (vinfo->inc_instr_idx >= insert_pos) vinfo->inc_instr_idx += 2;
            step_t = vl_t;
        } else if (vinfo->is_sve) {
            int cnt_t = zc_alloc_temp(c);
            Instr whilelt = {
                .op = OP_SVE_WHILELT,
                .temp_dest = TEMP_NONE,
                .temp_src1 = (uint32_t)cur_var_t,
                .temp_src2 = bound_t,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)esz,
                .imm64 = 0,
                .label_idx = 0
            };
            Instr get_cnt = {
                .op = OP_SVE_CNT,
                .temp_dest = (uint32_t)cnt_t,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = (int16_t)esz,
                .imm64 = 31,
                .label_idx = 0
            };
            ir_insert_at(c, (size_t)insert_pos, whilelt);
            ir_insert_at(c, (size_t)(insert_pos + 1), get_cnt);
            end_idx += 2;
            if (vinfo->inc_instr_idx >= insert_pos) vinfo->inc_instr_idx += 2;
            step_t = cnt_t;
        }

        int inc_idx = vinfo->inc_instr_idx;
        Instr *inc = ir_get(c, inc_idx);
        if (!inc) return false;
        if (inc->op == OP_INC || inc->op == OP_ADD_IMM) {
            inc->op = OP_ADD;
            inc->temp_src1 = (inc->temp_dest != TEMP_NONE) ? inc->temp_dest : (uint32_t)vinfo->loop_var_temp;
            inc->temp_src2 = (uint32_t)step_t;
            inc->imm = 0;
            inc->imm64 = 0;
        } else if (inc->op == OP_ADD) {
            if (vinfo->inc_use_src2) inc->temp_src1 = (uint32_t)step_t;
            else inc->temp_src2 = (uint32_t)step_t;
        } else {
            return false;
        }
    } else {
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
                if (vinfo->red_load_idx >= inc_idx) vinfo->red_load_idx++;
                if (vinfo->red_store_idx >= inc_idx) vinfo->red_store_idx++;
                if (vinfo->red_arith_idx >= inc_idx) vinfo->red_arith_idx++;
                Instr *inc_after = ir_get(c, inc_idx + 1);
                if (!inc_after) return false;
                if (vinfo->inc_use_src2) inc_after->temp_src1 = (uint32_t)step_t;
                else inc_after->temp_src2 = (uint32_t)step_t;
            }
        } else {
            return false;
        }
    }

    LoopCondDiamond diamonds[16];
    int diamond_count = 0;
    int v_zero = -1;
    if (vinfo->has_cond_select) {
        v_zero = zc_alloc_temp(c);
        Instr z_ins = {
            .op = OP_VXORPS,
            .temp_dest = (uint32_t)v_zero,
            .temp_src1 = (uint32_t)v_zero,
            .temp_src2 = (uint32_t)v_zero,
            .cond = 0,
            .flags_out = 0xFF,
            .flags_in = 0xFF,
            .imm = 0,
            .imm64 = 0,
            .label_idx = 0
        };
        ir_insert_at(c, (size_t)start_idx, z_ins);
        start_idx++;
        end_idx++;
        if (vinfo->inc_instr_idx >= start_idx - 1) vinfo->inc_instr_idx++;
        if (vinfo->test_instr_idx >= start_idx - 1) vinfo->test_instr_idx++;
        if (vinfo->cmp_instr_idx >= start_idx - 1) vinfo->cmp_instr_idx++;
        diamond_count = detect_loop_diamonds(c, start_idx, end_idx, vinfo->test_instr_idx, NULL, diamonds, 16);
    }

    /* Track scalar temp -> fresh untagged vector temp mapping so REG_CLASS_VEC
     * never aliases compacted REG_CLASS_GPR / REG_CLASS_XMM temp IDs. */
    static int32_t s_scalar_to_vec_map[65536];
    int32_t *scalar_to_vec_map = s_scalar_to_vec_map;
    memset(scalar_to_vec_map, 0xFF, sizeof(s_scalar_to_vec_map));

    for (int d = 0; d < diamond_count; d++) {
        uint32_t ops[4] = {
            (uint32_t)diamonds[d].then_val,
            (uint32_t)diamonds[d].else_val,
            ir_get(c, diamonds[d].cmp_idx)->temp_src1,
            ir_get(c, diamonds[d].cmp_idx)->temp_src2
        };
        int search_from[4] = {
            diamonds[d].then_end,
            (diamonds[d].else_end >= 0 ? diamonds[d].else_end : diamonds[d].end_label_idx),
            diamonds[d].cmp_idx,
            diamonds[d].cmp_idx
        };
        for (int op_idx = 0; op_idx < 4; op_idx++) {
            uint32_t t = ops[op_idx];
            if (t == TEMP_NONE || t >= 65536) continue;
            int64_t cv = 0;
            int def_k = -1;
            if (resolve_const_at(c, search_from[op_idx], t, &cv, &def_k) && cv == 0) {
                scalar_to_vec_map[t] = v_zero;
                if ((uint32_t)diamonds[d].then_val == t) diamonds[d].vec_then = v_zero;
                if ((uint32_t)diamonds[d].else_val == t) diamonds[d].vec_else = v_zero;
            }
        }
    }

    /* 2.5 Pre-classify address temps for loads and stores BEFORE modifying
     *     any instructions in the loop, avoiding mutation interference. */
    int loop_span = end_idx - start_idx + 1;
    typedef struct {
        int cls;
        int scale;
        int base_t;
        int idx_t;
    } LoopAddrInfo;
    LoopAddrInfo addr_info_buf[256];
    LoopAddrInfo *addr_info = (loop_span <= 256)
        ? addr_info_buf
        : (LoopAddrInfo *)calloc((size_t)loop_span, sizeof(LoopAddrInfo));
    if (loop_span <= 256) memset(addr_info_buf, 0, sizeof(addr_info_buf));

    for (int i = start_idx; i <= end_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        bool is_load = (ins->op == OP_LOAD_BYTE || ins->op == OP_LOAD_16 ||
                        ins->op == OP_LOAD_32 || ins->op == OP_FLOAD ||
                        ins->op == OP_VLOADSS || ins->op == OP_VLOADSD);
        bool is_store = (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
                         ins->op == OP_STORE_32 || ins->op == OP_FSTORE ||
                         ins->op == OP_VSTORESS || ins->op == OP_VSTORESD);
        if (!is_load && !is_store) continue;
        bool is_int_st = (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 || ins->op == OP_STORE_32);
        uint32_t addr = is_int_st ? ins->temp_src2 : ins->temp_src1;
        int sc = 0;
        int cls = classify_addr_temp(c, start_idx, end_idx, i, NULL, addr,
                                     vinfo->loop_var_temp, vinfo->counter_slot, 0, &sc, NULL, NULL);
        int base_t = -1, idx_t = -1, sc_idx = 0;
        if (cls == 2) {
            get_indexed_info(c, start_idx, end_idx, i, NULL, addr,
                             vinfo->loop_var_temp, vinfo->counter_slot, &base_t, &idx_t, &sc_idx);
        }
        int rel = i - start_idx;
        addr_info[rel].cls = cls;
        addr_info[rel].scale = sc;
        addr_info[rel].base_t = base_t;
        addr_info[rel].idx_t = idx_t;
    }

    int esz = (vinfo->elem_size == 8) ? 3 : (vinfo->elem_size == 4) ? 2 : (vinfo->elem_size == 2) ? 1 : 0;

    for (int i = start_idx; i <= end_idx; i++) {
        Instr *instr = ir_get(c, i);
        if (!instr || instr->op == OP_NOP || instr->op == OP_DEAD) continue;

        if (diamond_count > 0) {
            int d_idx = -1;
            for (int d = 0; d < diamond_count; d++) {
                if (i == diamonds[d].cmp_idx || i == diamonds[d].condset_idx ||
                    i == diamonds[d].je_idx || i == diamonds[d].jmp_idx ||
                    i == diamonds[d].else_label_idx || i == diamonds[d].end_label_idx ||
                    (i >= diamonds[d].then_start && i <= diamonds[d].then_end) ||
                    (diamonds[d].else_start >= 0 && i >= diamonds[d].else_start && i <= diamonds[d].else_end)) {
                    d_idx = d;
                    break;
                }
            }
            if (d_idx >= 0) {
                LoopCondDiamond *d = &diamonds[d_idx];
                if (i == d->cmp_idx) {
                    Instr *cm = instr;
                    uint32_t s1 = cm->temp_src1;
                    uint32_t s2 = cm->temp_src2;
                    int vec_s1 = (s1 < 65536 && scalar_to_vec_map[s1] >= 0) ? scalar_to_vec_map[s1] : v_zero;
                    int vec_s2 = (s2 < 65536 && scalar_to_vec_map[s2] >= 0) ? scalar_to_vec_map[s2] : v_zero;
                    int v_mask = zc_alloc_temp(c);
                    d->vec_mask = v_mask;
                    cm->op = OP_V128_CMP;
                    cm->temp_dest = (uint32_t)v_mask;
                    cm->temp_src1 = (uint32_t)vec_s1;
                    cm->temp_src2 = (uint32_t)vec_s2;
                    cm->imm = (int16_t)vinfo->elem_size;
                    cm->cond = (uint8_t)d->cond;
                    cm->imm64 = (int64_t)(d->is_fp ? 1 : 0);
                    cm->label_idx = 0;
                    cm->flags_out = 0xFF; cm->flags_in = 0xFF;
                    continue;
                }
                if (i == d->condset_idx || i == d->je_idx ||
                    (d->jmp_idx >= 0 && i == d->jmp_idx) ||
                    (d->else_label_idx >= 0 && i == d->else_label_idx)) {
                    instr->op = OP_NOP;
                    continue;
                }
                if (i >= d->then_start && i <= d->then_end) {
                    if ((instr->op == OP_MOV_REG || instr->op == OP_FMOV) && (int)instr->temp_dest == d->res_temp) {
                        instr->op = OP_NOP;
                        continue;
                    }
                }
                if (d->else_start >= 0 && i >= d->else_start && i <= d->else_end) {
                    if ((instr->op == OP_MOV_REG || instr->op == OP_FMOV) && (int)instr->temp_dest == d->res_temp) {
                        instr->op = OP_NOP;
                        continue;
                    }
                }
                if (i == d->end_label_idx) {
                    int vec_then = (d->vec_then >= 0) ? d->vec_then :
                        ((d->then_val < 65536 && scalar_to_vec_map[d->then_val] >= 0) ? scalar_to_vec_map[d->then_val] : v_zero);
                    int vec_else = (d->vec_else >= 0) ? d->vec_else :
                        ((d->else_val < 65536 && scalar_to_vec_map[d->else_val] >= 0) ? scalar_to_vec_map[d->else_val] : v_zero);
                    int v_res = zc_alloc_temp(c);
                    d->vec_res = v_res;
                    instr->op = OP_V128_SELECT;
                    instr->temp_dest = (uint32_t)v_res;
                    instr->temp_src1 = (uint32_t)vec_then;
                    instr->temp_src2 = (uint32_t)vec_else;
                    instr->imm64 = (int64_t)d->vec_mask;
                    instr->imm = (int16_t)vinfo->elem_size;
                    instr->label_idx = 0;
                    instr->cond = 0;
                    instr->flags_out = 0xFF; instr->flags_in = 0xFF;
                    if (d->res_temp < 65536) scalar_to_vec_map[d->res_temp] = v_res;
                    continue;
                }
            }
        }

        if (vinfo->is_reduction) {
            bool is_red_load = (i == vinfo->red_load_idx);
            bool is_red_store = (i == vinfo->red_store_idx);
            if (vinfo->reduction_slot != 0x7fffffff) {
                if (instr->op == OP_MOV && instr->temp_src1 == TEMP_NONE && instr->imm == vinfo->reduction_slot) is_red_load = true;
                if ((instr->op == OP_MOV_MEM || instr->op == OP_FSTORE) && instr->imm == vinfo->reduction_slot) is_red_store = true;
            }
            if (instr->op == OP_LOAD_32) is_red_load = false;
            if (is_red_load || is_red_store) {
                instr->op = OP_NOP;
                continue;
            }
            if (i == vinfo->red_arith_idx) {
                int vec_elem = -1;
                if (zc_temp_is_user(instr->temp_src1) && instr->temp_src1 < 65536 && scalar_to_vec_map[instr->temp_src1] >= 0) {
                    vec_elem = scalar_to_vec_map[instr->temp_src1];
                } else if (zc_temp_is_user(instr->temp_src2) && instr->temp_src2 < 65536 && scalar_to_vec_map[instr->temp_src2] >= 0) {
                    vec_elem = scalar_to_vec_map[instr->temp_src2];
                }
                int vec_op;
                if (vinfo->is_scalable) {
                    vec_op = vinfo->is_rvv ? get_rvv_op(instr->op) : get_sve_op(instr->op);
                } else {
                    vec_op = vinfo->is_reduction_fp ? (vinfo->is_double ? OP_VADDPD : OP_VADDPS) : OP_VPADDD;
                }
                instr->op = (uint16_t)vec_op;
                instr->temp_dest = (uint32_t)v_acc;
                instr->temp_src1 = (uint32_t)v_acc;
                instr->temp_src2 = (uint32_t)vec_elem;
                instr->imm = vinfo->is_scalable ? (int16_t)esz : 0;
                instr->imm64 = vinfo->is_scalable ? (vinfo->is_reduction_fp ? 1 : 0) : 0;
                instr->label_idx = 0;
                instr->cond = 0;
                instr->width = 0;
                instr->flags_out = 0xFF;
                instr->flags_in = 0xFF;
                continue;
            }
        }

        /* Scalar loads -> vector load into fresh vector temp */
        if (instr->op == OP_LOAD_BYTE || instr->op == OP_LOAD_16 ||
            instr->op == OP_LOAD_32 || instr->op == OP_FLOAD ||
            instr->op == OP_VLOADSS || instr->op == OP_VLOADSD) {
            if (!zc_temp_is_user(instr->temp_dest) || instr->temp_dest >= 65536) continue;
            uint32_t old_dest = instr->temp_dest;
            int vec_temp = zc_alloc_temp(c);

            int rel = i - start_idx;
            int cls = addr_info[rel].cls;
            int scale = addr_info[rel].scale;
            bool is_strided = (cls == 1 && scale > vinfo->elem_size);

            if (cls == 2) {
                int base_t = addr_info[rel].base_t;
                int idx_t = addr_info[rel].idx_t;
                int vec_idx = (idx_t >= 0 && idx_t < 65536) ? scalar_to_vec_map[idx_t] : -1;
                if (vec_idx >= 0) {
                    instr->op = vinfo->is_rvv ? OP_RVV_VLUXEI : OP_V128_GATHER;
                    instr->temp_dest = (uint32_t)vec_temp;
                    instr->temp_src1 = (uint32_t)base_t;
                    instr->temp_src2 = (uint32_t)vec_idx;
                    instr->imm = (int16_t)vinfo->elem_size;
                    instr->imm64 = 0;
                } else {
                    instr->op = OP_VLOAD;
                    instr->temp_dest = (uint32_t)vec_temp;
                    instr->temp_src2 = TEMP_NONE;
                    instr->imm64 = 0;
                }
            } else if (is_strided) {
                instr->op = OP_V128_LOAD_STRIDED;
                instr->temp_dest = (uint32_t)vec_temp;
                instr->temp_src2 = TEMP_NONE;
                instr->imm = (int16_t)scale;
                instr->imm64 = (int64_t)vinfo->elem_size;
            } else if (vinfo->is_scalable && !vinfo->has_strided_access) {
                instr->op = vinfo->is_rvv ? OP_RVV_VLE : OP_SVE_LD1;
                instr->temp_dest = (uint32_t)vec_temp;
                instr->temp_src2 = TEMP_NONE;
                instr->imm = (int16_t)esz;
                instr->imm64 = 0;
            } else {
                instr->op = is_512 ? OP_V512_LOAD : (is_256 ? OP_V256_LOAD : OP_VLOAD);
                instr->temp_dest = (uint32_t)vec_temp;
                instr->temp_src2 = TEMP_NONE;
                instr->imm64 = 0;
            }
            instr->label_idx = 0;
            instr->cond = 0;
            instr->width = 0;
            instr->flags_out = 0xFF;
            instr->flags_in = 0xFF;
            scalar_to_vec_map[old_dest] = vec_temp;
            if (diamond_count > 0) {
                for (int d = 0; d < diamond_count; d++) {
                    if (i >= diamonds[d].then_start && i <= diamonds[d].then_end) {
                        if (old_dest == (uint32_t)diamonds[d].then_val) diamonds[d].vec_then = vec_temp;
                    }
                    if (diamonds[d].else_start >= 0 && i >= diamonds[d].else_start && i <= diamonds[d].else_end) {
                        if (old_dest == (uint32_t)diamonds[d].else_val) diamonds[d].vec_else = vec_temp;
                    }
                }
            }
            continue;
        }

        /* Forward mapped vector values through casts and copies */
        if (instr->op == OP_CVTSS2SD || instr->op == OP_CVTSD2SS || instr->op == OP_MOV_REG) {
            if (zc_temp_is_user(instr->temp_src1) && instr->temp_src1 < 65536 &&
                scalar_to_vec_map[instr->temp_src1] >= 0 &&
                zc_temp_is_user(instr->temp_dest) && instr->temp_dest < 65536) {
                scalar_to_vec_map[instr->temp_dest] = scalar_to_vec_map[instr->temp_src1];
                if (diamond_count > 0) {
                    for (int d = 0; d < diamond_count; d++) {
                        if (i >= diamonds[d].then_start && i <= diamonds[d].then_end) {
                            if (instr->temp_dest == (uint32_t)diamonds[d].then_val) {
                                diamonds[d].vec_then = scalar_to_vec_map[instr->temp_src1];
                            }
                        }
                        if (diamonds[d].else_start >= 0 && i >= diamonds[d].else_start && i <= diamonds[d].else_end) {
                            if (instr->temp_dest == (uint32_t)diamonds[d].else_val) {
                                diamonds[d].vec_else = scalar_to_vec_map[instr->temp_src1];
                            }
                        }
                    }
                }
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
                if (instr->temp_dest != TEMP_NONE && instr->temp_src1 != TEMP_NONE &&
                    instr->temp_dest < 65536 && instr->temp_dest != instr->temp_src1) {
                    scalar_to_vec_map[instr->temp_dest] = scalar_to_vec_map[instr->temp_src1];
                }
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
                    bool is_fp = (instr->op == OP_FADD || instr->op == OP_FSUB || instr->op == OP_FMUL || instr->op == OP_FDIV);
                    int vec_op;
                    if (vinfo->is_scalable) {
                        vec_op = vinfo->is_rvv ? get_rvv_op(instr->op) : get_sve_op(instr->op);
                    } else {
                        vec_op = get_vec_op(instr->op, vinfo->is_double, vinfo->is_int32, vinfo->is_int16, vinfo->is_int8, is_256, is_512);
                    }
                    int vec_dest = zc_alloc_temp(c);
                    scalar_to_vec_map[instr->temp_dest] = vec_dest;
                    if (diamond_count > 0) {
                        for (int d = 0; d < diamond_count; d++) {
                            if (i >= diamonds[d].then_start && i <= diamonds[d].then_end) {
                                if (instr->temp_dest == (uint32_t)diamonds[d].then_val) diamonds[d].vec_then = vec_dest;
                            }
                            if (diamonds[d].else_start >= 0 && i >= diamonds[d].else_start && i <= diamonds[d].else_end) {
                                if (instr->temp_dest == (uint32_t)diamonds[d].else_val) diamonds[d].vec_else = vec_dest;
                            }
                        }
                    }

                    instr->op = (uint16_t)vec_op;
                    instr->temp_dest = (uint32_t)vec_dest;
                    instr->temp_src1 = (uint32_t)vec_src1;
                    instr->temp_src2 = (uint32_t)vec_src2;
                    instr->imm = vinfo->is_scalable ? (int16_t)esz : 0;
                    instr->imm64 = vinfo->is_scalable ? (is_fp ? 1 : 0) : 0;
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
        if (instr->op == OP_STORE_BYTE || instr->op == OP_STORE_16 ||
            instr->op == OP_STORE_32 || instr->op == OP_FSTORE ||
            instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
            bool is_int_store = (instr->op == OP_STORE_BYTE || instr->op == OP_STORE_16 || instr->op == OP_STORE_32);
            uint32_t scalar_val_temp = is_int_store ? instr->temp_src1 : instr->temp_src2;
            uint32_t scalar_addr_temp = is_int_store ? instr->temp_src2 : instr->temp_src1;
            int vec_value = (scalar_val_temp < 65536) ? scalar_to_vec_map[scalar_val_temp] : -1;
            if (vec_value < 0) continue;

            int rel = i - start_idx;
            int cls = addr_info[rel].cls;
            int scale = addr_info[rel].scale;
            bool is_strided = (cls == 1 && scale > vinfo->elem_size);

            if (cls == 2) {
                int base_t = addr_info[rel].base_t;
                int idx_t = addr_info[rel].idx_t;
                int vec_idx = (idx_t >= 0 && idx_t < 65536) ? scalar_to_vec_map[idx_t] : -1;
                if (vec_idx >= 0) {
                    instr->op = vinfo->is_rvv ? OP_RVV_VSUXEI : OP_V128_SCATTER;
                    instr->temp_dest = (uint32_t)vec_value;
                    instr->temp_src1 = (uint32_t)base_t;
                    instr->temp_src2 = (uint32_t)vec_idx;
                    instr->imm = (int16_t)vinfo->elem_size;
                    instr->imm64 = 0;
                } else {
                    instr->op = OP_VSTORE;
                    instr->temp_dest = TEMP_NONE;
                    instr->temp_src1 = scalar_addr_temp;
                    instr->temp_src2 = (uint32_t)vec_value;
                    instr->imm64 = 0;
                }
            } else if (is_strided) {
                instr->op = OP_V128_STORE_STRIDED;
                instr->temp_dest = TEMP_NONE;
                instr->temp_src1 = scalar_addr_temp;
                instr->temp_src2 = (uint32_t)vec_value;
                instr->imm = (int16_t)scale;
                instr->imm64 = (int64_t)vinfo->elem_size;
            } else if (vinfo->is_scalable && !vinfo->has_strided_access) {
                instr->op = vinfo->is_rvv ? OP_RVV_VSE : OP_SVE_ST1;
                instr->temp_dest = TEMP_NONE;
                instr->temp_src1 = scalar_addr_temp;
                instr->temp_src2 = (uint32_t)vec_value;
                instr->imm = (int16_t)esz;
                instr->imm64 = 0;
            } else {
                instr->op = is_512 ? OP_V512_STORE : (is_256 ? OP_V256_STORE : OP_VSTORE);
                instr->temp_dest = TEMP_NONE;
                instr->temp_src1 = scalar_addr_temp;
                instr->temp_src2 = (uint32_t)vec_value;
                instr->imm64 = 0;
            }
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

    if (vinfo->is_reduction) {
        int ins_pos = end_idx + 1;
        Instr lbl = {
            .op = OP_LABEL,
            .temp_dest = TEMP_NONE,
            .temp_src1 = TEMP_NONE,
            .temp_src2 = TEMP_NONE,
            .label_idx = (uint16_t)red_exit_label,
            .flags_out = 0xFF,
            .flags_in = 0xFF
        };
        ir_insert_at(c, (size_t)ins_pos++, lbl);

        int scalar_red_t = zc_alloc_temp(c);
        int esz = vinfo->is_double ? 3 : 2;
        int is_fp = vinfo->is_reduction_fp ? 1 : 0;
        Instr hadd = {
            .op = OP_V128_HADD,
            .temp_dest = (uint32_t)scalar_red_t,
            .temp_src1 = (uint32_t)v_acc,
            .temp_src2 = TEMP_NONE,
            .cond = 0,
            .flags_out = 0xFF,
            .flags_in = 0xFF,
            .imm = (int16_t)esz,
            .imm64 = is_fp,
            .label_idx = 0
        };
        ir_insert_at(c, (size_t)ins_pos++, hadd);

        if (vinfo->reduction_slot != 0x7fffffff) {
            if (is_fp) {
                int addr_t = zc_alloc_temp(c);
                Instr lea = {
                    .op = OP_LEA,
                    .temp_dest = (uint32_t)addr_t,
                    .temp_src1 = TEMP_NONE,
                    .temp_src2 = TEMP_NONE,
                    .imm = (int16_t)vinfo->reduction_slot,
                    .imm64 = vinfo->reduction_slot,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, lea);

                int acc_old = zc_alloc_temp(c);
                Instr fload = {
                    .op = vinfo->is_double ? OP_VLOADSD : OP_VLOADSS,
                    .temp_dest = (uint32_t)acc_old,
                    .temp_src1 = (uint32_t)addr_t,
                    .temp_src2 = TEMP_NONE,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, fload);

                int acc_new = zc_alloc_temp(c);
                Instr fadd = {
                    .op = OP_FADD,
                    .temp_dest = (uint32_t)acc_new,
                    .temp_src1 = (uint32_t)acc_old,
                    .temp_src2 = (uint32_t)scalar_red_t,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, fadd);

                Instr fstore = {
                    .op = vinfo->is_double ? OP_VSTORESD : OP_VSTORESS,
                    .temp_dest = TEMP_NONE,
                    .temp_src1 = (uint32_t)addr_t,
                    .temp_src2 = (uint32_t)acc_new,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, fstore);
            } else {
                int acc_old = zc_alloc_temp(c);
                Instr load_m = {
                    .op = OP_MOV,
                    .temp_dest = (uint32_t)acc_old,
                    .temp_src1 = TEMP_NONE,
                    .temp_src2 = TEMP_NONE,
                    .imm = (int16_t)vinfo->reduction_slot,
                    .imm64 = vinfo->reduction_slot,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, load_m);

                int acc_new = zc_alloc_temp(c);
                Instr add_m = {
                    .op = OP_ADD,
                    .temp_dest = (uint32_t)acc_new,
                    .temp_src1 = (uint32_t)acc_old,
                    .temp_src2 = (uint32_t)scalar_red_t,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, add_m);

                Instr store_m = {
                    .op = OP_MOV_MEM,
                    .temp_dest = TEMP_NONE,
                    .temp_src1 = (uint32_t)acc_new,
                    .temp_src2 = TEMP_NONE,
                    .imm = (int16_t)vinfo->reduction_slot,
                    .imm64 = vinfo->reduction_slot,
                    .flags_out = 0xFF,
                    .flags_in = 0xFF
                };
                ir_insert_at(c, (size_t)ins_pos++, store_m);
            }
        } else if (vinfo->reduction_temp != TEMP_NONE) {
            int acc_new = zc_alloc_temp(c);
            Instr add_r = {
                .op = is_fp ? OP_FADD : OP_ADD,
                .temp_dest = (uint32_t)acc_new,
                .temp_src1 = (uint32_t)vinfo->reduction_temp,
                .temp_src2 = (uint32_t)scalar_red_t,
                .flags_out = 0xFF,
                .flags_in = 0xFF
            };
            ir_insert_at(c, (size_t)ins_pos++, add_r);

            Instr mov_r = {
                .op = OP_MOV_REG,
                .temp_dest = (uint32_t)vinfo->reduction_temp,
                .temp_src1 = (uint32_t)acc_new,
                .temp_src2 = TEMP_NONE,
                .flags_out = 0xFF,
                .flags_in = 0xFF
            };
            ir_insert_at(c, (size_t)ins_pos++, mov_r);
        }

        int target_label = vinfo->needs_peel ? rem_head_label : orig_exit_label;
        Instr jmp_target = {
            .op = OP_JMP,
            .temp_dest = TEMP_NONE,
            .temp_src1 = TEMP_NONE,
            .temp_src2 = TEMP_NONE,
            .imm = 0,
            .imm64 = 0,
            .label_idx = (uint16_t)target_label,
            .flags_out = 0xFF,
            .flags_in = 0xFF
        };
        ir_insert_at(c, (size_t)ins_pos++, jmp_target);

        c->cfg_cache.valid = false;
    }

    if (addr_info != addr_info_buf) free(addr_info);
    return true;
}

/**
 * Main auto-vectorization pass
 */
void opt_auto_vectorize(ZeroCCCompiler *c) {
    if (!c || !BE(c) || !BE(c)->emit_vload || !BE(c)->emit_vstore ||
        !BE(c)->emit_vaddps || !zc_target_has_simd(c)) {
        return;
    }
    ZeroCCISA isa = c->regalloc.target_isa ? c->regalloc.target_isa : c->isa;

    VectorizationStateFull state = {0};
    vectorization_init((VectorizationState *)&state, (int)isa);
    state.comp = c;
    if (c->has_avx512f && isa == ISA_X86_64) {
        state.vector_width = 16;
        for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
            state.loops[i].vector_width = 16;
        }
    } else if (c->has_sve && isa == ISA_AARCH64) {
        state.vector_width = 16;
        for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
            state.loops[i].vector_width = 16;
        }
    } else if (c->has_rvv && (isa == ISA_RISCV64 || isa == ISA_RISCV32)) {
        state.vector_width = 4;
        for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
            state.loops[i].vector_width = 4;
        }
    } else if (c->has_avx && isa == ISA_X86_64) {
        state.vector_width = 8;
        for (int i = 0; i < MAX_VECTOR_LOOPS; i++) {
            state.loops[i].vector_width = 8;
        }
    }

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
