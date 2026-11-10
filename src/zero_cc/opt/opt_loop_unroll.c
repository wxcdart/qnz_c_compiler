/*
 * Loop full-unrolling pass (Sprint 150, development plan item 3).
 *
 * See opt_loop_unroll.h for the transform contract and gate list.
 */

#include "zero_cc/opt/opt_loop_unroll.h"
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_constant_fold.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/types/symbols.h"
#include <stdlib.h>
#include <string.h>

#define LU_MAX_TRIP  8    /* trip-count cap for full unroll (plan item 3) */
#define LU_MAX_BODY  64   /* max loop span (header..backedge, inclusive) */
#define LU_TRACE_MAX 12   /* value-trace walk depth cap */

enum { LU_VAL_OTHER = 0, LU_VAL_CONST, LU_VAL_SLOT, LU_VAL_BASE };

typedef struct {
    int H;          /* header index (OP_LABEL) */
    int E;          /* backedge index (OP_JMP to H) */
    int bh_end;     /* header block terminal (the loop test JE/JNE) */
    int bu_start;   /* backedge block start (nearest leader <= E) */
    int step_label; /* label at bu_start when the backedge block is labeled, else -1 */
    int exit_label; /* the test's exit label */
    int counter_kind;   /* 0 = stack slot, 1 = promoted temp */
    int64_t counter_slot;
    int counter_temp;
    int64_t init;       /* counter value on loop entry */
    int64_t bound;      /* the constant bound in the loop test */
    int cc;             /* the CONTINUE condition (after JNE negation) */
    int trip;           /* verified constant trip count */
} LUPlan;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int lu_is_jump(uint8_t op) {
    switch (op) {
        case OP_JMP: case OP_JE: case OP_JNE: case OP_JL: case OP_JLE:
        case OP_JG: case OP_JCOND: case OP_JMP_LABEL: case OP_JMP_INDIRECT:
            return 1;
        default:
            return 0;
    }
}

/* Side-effecting opcodes that must never be silently deleted (header
 * purity) or replicated (span gate). */
static int lu_has_side_effect(uint8_t op) {
    switch (op) {
        case OP_MOV_MEM:
        case OP_STORE_BYTE: case OP_STORE_16: case OP_STORE_32: case OP_STORE_64:
        case OP_FSTORE: case OP_VSTORE: case OP_VSTORESS: case OP_VSTORESD:
        case OP_VSTORESS_RAW: case OP_STORE128:
        case OP_ATOMIC_XADD: case OP_ATOMIC_CMPXCHG: case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG: case OP_ATOMIC_OR: case OP_ATOMIC_AND: case OP_ATOMIC_XOR:
        case OP_MFENCE: case OP_TRAP: case OP_OVF_TRAP:
        case OP_CALL: case OP_CALL_INDIRECT: case OP_SYSCALL: case OP_CLONE:
        case OP_ASM: case OP_VLA_ALLOC:
        case OP_PROLOGUE: case OP_EPILOGUE:
        case OP_PARAM_SAVE_INT: case OP_PARAM_SAVE_FP: case OP_PARAM_LOAD:
        case OP_VARIADIC_SPILL:
        case OP_SETJMP: case OP_LONGJMP: case OP_NONLOCAL_GOTO:
        case OP_PUSH: case OP_POP:
        case OP_DEAD:
            return 1;
        default:
            return 0;
    }
}

/* Negate a compare condition (JNE-form tests). */
static int lu_negate_cc(int cc) {
    switch (cc) {
        case CC_LT: return CC_GE;
        case CC_GE: return CC_LT;
        case CC_LE: return CC_GT;
        case CC_GT: return CC_LE;
        default:    return cc;
    }
}

/*
 * Resolve a temp to a compile-time constant.  The constant-fold map
 * (ir_is_constant) is not guaranteed to cover every constant temp by the
 * time this pass runs (e.g. a step constant LICM hoisted but the fold map
 * not rebuilt yet for this exact layout), so fall back to the temp's
 * MOV_IMM definition, mirroring the auto-vectorizer's resolve_const.
 */
static int lu_resolve_const(ZeroCCCompiler *c, int t, int64_t *val_out) {
    if (t == TEMP_NONE) return 0;
    if (ir_is_constant(t)) { *val_out = ir_get_constant(t); return 1; }
    int count = (int)ir_get_count(c);
    for (int k = 0; k < count; k++) {
        Instr *d = ir_get(c, k);
        if (d && (int)d->temp_dest == t && d->op == OP_MOV_IMM) {
            *val_out = d->imm64;
            return 1;
        }
    }
    return 0;
}

/*
 * Walk backwards from the LAST definition of `temp` in [lo, hi] through
 * value-preserving definitions (MOV_REG, in-place SEXT/ZEXT) and
 * classify the value:
 *   LU_VAL_CONST - *val_out = the constant (MOV_IMM def or const temp)
 *   LU_VAL_SLOT  - *val_out = the frame offset (OP_MOV stack load)
 *   LU_VAL_BASE  - *val_out = the base temp id (a non-constant value)
 *   LU_VAL_OTHER - anything else, or no definition in the window
 */
static int lu_trace_value(ZeroCCCompiler *c, int lo, int hi, int temp, int depth, int64_t *val_out) {
    if (depth > LU_TRACE_MAX || temp == TEMP_NONE || lo > hi) return LU_VAL_OTHER;
    int64_t cv;
    if (lu_resolve_const(c, temp, &cv)) { *val_out = cv; return LU_VAL_CONST; }
    for (int k = hi; k >= lo; k--) {
        Instr *d = ir_get(c, k);
        if (!d || (int)d->temp_dest != temp) continue;
        switch (d->op) {
            case OP_MOV_IMM:
                *val_out = d->imm64;
                return LU_VAL_CONST;
            case OP_MOV:
                /* stack load: temp = [frame + imm64], src1 unused */
                if (d->temp_src1 == TEMP_NONE) { *val_out = d->imm64; return LU_VAL_SLOT; }
                return LU_VAL_OTHER;
            case OP_MOV_REG:
                return lu_trace_value(c, lo, k - 1, (int)d->temp_src1, depth + 1, val_out);
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (d->temp_src1 == d->temp_dest) continue;  /* in-place: keep walking */
                return LU_VAL_OTHER;
            default:
                return LU_VAL_BASE;  /* a real (non-constant) definition */
        }
    }
    return LU_VAL_OTHER;  /* no definition in the window */
}

/* ------------------------------------------------------------------ */
/* Label map (label id -> instruction position)                        */
/* ------------------------------------------------------------------ */

typedef struct { uint16_t label; int pos; } LULabel;

static int lu_label_cmp(const void *a, const void *b) {
    const LULabel *la = (const LULabel *)a;
    const LULabel *lb = (const LULabel *)b;
    return (int)la->label - (int)lb->label;
}

static int lu_label_pos(const LULabel *map, int n, uint16_t label) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (map[mid].label == label) return map[mid].pos;
        if (map[mid].label < label) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* Build the sorted label->position map for the current IR.  Returns the
 * entry count (0 if nothing to map); *map_out is malloc'd and owned by
 * the caller. */
static int lu_build_label_map(ZeroCCCompiler *c, LULabel **map_out) {
    int count = (int)ir_get_count(c);
    LULabel *map = (LULabel *)malloc((size_t)(count > 0 ? count : 1) * sizeof(LULabel));
    if (!map) { *map_out = NULL; return 0; }
    int n = 0;
    for (int i = 0; i < count; i++) {
        Instr *x = ir_get(c, i);
        if (x && x->op == OP_LABEL) {
            map[n].label = x->label_idx;
            map[n].pos = i;
            n++;
        }
    }
    if (n > 1) qsort(map, (size_t)n, sizeof(LULabel), lu_label_cmp);
    *map_out = map;
    return n;
}

/* ------------------------------------------------------------------ */
/* Counter increment verification                                      */
/* ------------------------------------------------------------------ */

/*
 * Slot-form increment: within [bu_start, store_idx) the counter slot S
 * must be reloaded into a temp, incremented by exactly +1 (through an
 * in-place-extend-preserving chain), and the store at store_idx must
 * write that incremented value back.  Independent instructions (other
 * step-clause work) that don't touch the chain are allowed.
 */
static bool lu_slot_increment_ok(ZeroCCCompiler *c, int bu_start, int store_idx, int64_t S) {
    /* locate the reload of S */
    int t1 = -1, load_idx = -1;
    for (int k = bu_start; k < store_idx; k++) {
        Instr *x = ir_get(c, k);
        if (x && x->op == OP_MOV && x->temp_src1 == TEMP_NONE &&
            x->imm64 == S && x->temp_dest != TEMP_NONE) {
            t1 = (int)x->temp_dest;
            load_idx = k;
        }
    }
    if (t1 < 0 || load_idx < 0) return false;

    int cur = t1;
    int delta = 0;  /* value(cur) = [S] + delta (constant) */
    for (int k = load_idx + 1; k < store_idx; k++) {
        Instr *x = ir_get(c, k);
        if (!x) return false;
        uint8_t op = x->op;
        int d = (int)x->temp_dest, s1 = (int)x->temp_src1, s2 = (int)x->temp_src2;
        if (d != cur && s1 != cur && s2 != cur) continue;  /* independent op */
        int64_t cv;
        switch (op) {
            case OP_ADD:
                if (s1 == cur && s2 != cur && lu_resolve_const(c, s2, &cv) && cv == 1) { cur = d; delta = 1; continue; }
                if (s2 == cur && s1 != cur && lu_resolve_const(c, s1, &cv) && cv == 1) { cur = d; delta = 1; continue; }
                return false;
            case OP_ADD_IMM:
                if (d == cur && s1 == cur && (int)x->imm == 1) { delta = 1; continue; }
                return false;
            case OP_INC:
                if (d == cur && (s1 == cur || s1 == TEMP_NONE)) { delta = 1; continue; }
                return false;
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (d == cur && s1 == cur) continue;  /* in-place extend */
                return false;
            case OP_MOV_REG:
                if (d == cur && s1 == cur) continue;
                return false;
            default:
                return false;
        }
    }
    if (delta != 1) return false;
    Instr *st = ir_get(c, store_idx);
    return st && (int)st->temp_src1 == cur;
}

/*
 * Temp-form increment: the loop's single in-span definition of `iv`
 * (at def_idx, in the backedge block) must compute iv + 1.
 */
static bool lu_temp_increment_ok(ZeroCCCompiler *c, int bu_start, int def_idx, int iv) {
    Instr *x = ir_get(c, def_idx);
    if (!x || (int)x->temp_dest != iv) return false;
    int64_t cv;
    switch (x->op) {
        case OP_INC:
            return (int)x->temp_src1 == iv || x->temp_src1 == TEMP_NONE;
        case OP_ADD_IMM:
            return (int)x->temp_src1 == iv && (int)x->imm == 1;
        case OP_ADD: {
            int s1 = (int)x->temp_src1, s2 = (int)x->temp_src2;
            if (s1 == iv && s2 != iv && lu_resolve_const(c, s2, &cv) && cv == 1) return true;
            if (s2 == iv && s1 != iv && lu_resolve_const(c, s1, &cv) && cv == 1) return true;
            return false;
        }
        case OP_MOV_REG: {
            /* two-step: iv = t with t = iv + 1 (t's defining ADD in the
             * backedge block, after iv's last use-free point) */
            int t = (int)x->temp_src1;
            if (t == iv) return false;
            for (int k = def_idx - 1; k >= bu_start; k--) {
                Instr *y = ir_get(c, k);
                if (!y || (int)y->temp_dest != t) continue;
                int y1 = (int)y->temp_src1, y2 = (int)y->temp_src2;
                bool ok = (y->op == OP_ADD) &&
                    (((y1 == iv && y2 != iv && lu_resolve_const(c, y2, &cv) && cv == 1) ||
                     (y2 == iv && y1 != iv && lu_resolve_const(c, y1, &cv) && cv == 1)));
                return ok;  /* t's last definition decides */
            }
            return false;
        }
        default:
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* Loop qualification analysis                                         */
/* ------------------------------------------------------------------ */

static bool lu_analyze(ZeroCCCompiler *c, LoopInfo *loop, LUPlan *plan) {
    int count = (int)ir_get_count(c);
    int H = loop->header_idx;
    int E = loop->exit_idx;
    if (H < 0 || E < 0 || E >= count || E < H) return false;
    if (E - H + 1 > LU_MAX_BODY) return false;

    memset(plan, 0, sizeof(*plan));
    plan->H = H;
    plan->E = E;
    plan->step_label = -1;

    LULabel *lmap = NULL;
    int lmap_n = lu_build_label_map(c, &lmap);
    bool ok = false;

    Instr *h = ir_get(c, H);
    if (lmap_n > 0 && h && h->op == OP_LABEL) {
        Instr *be = ir_get(c, E);
        if (be && be->op == OP_JMP && lu_label_pos(lmap, lmap_n, be->label_idx) == H) {
            /* Header block terminal: first jump/terminal after the label. */
            int bh_end = -1;
            for (int i = H + 1; i <= E; i++) {
                Instr *x = ir_get(c, i);
                if (!x) continue;
                if (lu_is_jump(x->op) || x->op == OP_RET || x->op == OP_EPILOGUE) {
                    bh_end = i;
                    break;
                }
            }
            if (bh_end > H) {
                Instr *t = ir_get(c, bh_end);
                int exit_pos = (t && (t->op == OP_JE || t->op == OP_JNE))
                    ? lu_label_pos(lmap, lmap_n, t->label_idx) : -1;
                if (exit_pos > E) {
                    plan->bh_end = bh_end;
                    plan->exit_label = t->label_idx;

                    /* Header block purity: between the label and the test
                     * there may be no label/jump/terminal, no side effect
                     * (stores, calls, zero_san traps - the whole test is
                     * deleted per copy), and no header-defined temp that
                     * survives past the header. */
                    bool hdr_ok = true;
                    for (int i = H + 1; i < bh_end && hdr_ok; i++) {
                        Instr *x = ir_get(c, i);
                        if (!x) continue;
                        if (x->op == OP_LABEL || lu_is_jump(x->op) ||
                            x->op == OP_RET || x->op == OP_EPILOGUE ||
                            lu_has_side_effect(x->op)) {
                            hdr_ok = false;
                        }
                    }
                    if (hdr_ok) {
                        for (int i = H + 1; i < bh_end && hdr_ok; i++) {
                            Instr *x = ir_get(c, i);
                            if (!x || x->temp_dest == TEMP_NONE) continue;
                            int d = (int)x->temp_dest;
                            for (int j = bh_end + 1; j < count; j++) {
                                Instr *u = ir_get(c, j);
                                if (!u) continue;
                                if ((int)u->temp_src1 == d || (int)u->temp_src2 == d) {
                                    hdr_ok = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (hdr_ok) {
                        /* Span gate: nothing that unrolling cannot replicate
                         * exactly, nothing that would change meaning when the
                         * loop test disappears. */
                        for (int i = H; i <= E && hdr_ok; i++) {
                            Instr *x = ir_get(c, i);
                            if (!x) continue;
                            switch (x->op) {
                                case OP_CALL: case OP_CALL_INDIRECT:
                                case OP_SYSCALL: case OP_CLONE: case OP_ASM:
                                case OP_SETJMP: case OP_LONGJMP:
                                case OP_NONLOCAL_GOTO: case OP_JMP_INDIRECT:
                                case OP_LEA_LABEL: case OP_VLA_ALLOC:
                                case OP_ATOMIC_XADD: case OP_ATOMIC_CMPXCHG:
                                case OP_ATOMIC_CMPXCHG_BOOL: case OP_ATOMIC_XCHG:
                                case OP_ATOMIC_OR: case OP_ATOMIC_AND:
                                case OP_ATOMIC_XOR: case OP_MFENCE: case OP_TRAP:
                                case OP_PROLOGUE: case OP_EPILOGUE:
                                    hdr_ok = false;
                                    break;
                                default:
                                    break;
                            }
                        }
                    }

                    /* Backedge block start: nearest leader at or before E. */
                    int bu_start = H;
                    for (int i = E - 1; i >= H; i--) {
                        Instr *x = ir_get(c, i);
                        if (!x) continue;
                        if (x->op == OP_LABEL) { bu_start = i; break; }
                        if (lu_is_jump(x->op) || x->op == OP_RET ||
                            x->op == OP_EPILOGUE) { bu_start = i + 1; break; }
                    }
                    if (hdr_ok) {
                        plan->bu_start = bu_start;
                        Instr *bs = ir_get(c, bu_start);
                        plan->step_label = (bs && bs->op == OP_LABEL)
                            ? (int)bs->label_idx : -1;

                        /* Back-jumps: only "continue" edges - to the header
                         * (any body position) or to the for-step label (from
                         * the body proper, before the step block).  Anything
                         * else backward is a nested loop. */
                        for (int i = H + 1; i < E && hdr_ok; i++) {
                            Instr *x = ir_get(c, i);
                            if (!x || !lu_is_jump(x->op)) continue;
                            int p = lu_label_pos(lmap, lmap_n, x->label_idx);
                            if (p < 0 || p >= i) continue;  /* forward edge */
                            if (i <= bh_end) { hdr_ok = false; break; }
                            if (p == H) continue;
                            if (p == bu_start && plan->step_label >= 0 && i < bu_start) continue;
                            hdr_ok = false;
                        }

                        /* Side entries: a jump from outside the span into it
                         * would re-enter the loop mid-way (unrolling changes
                         * how many iterations such an entry sees). */
                        for (int i = 0; i < count && hdr_ok; i++) {
                            if (i >= H && i <= E) continue;
                            Instr *x = ir_get(c, i);
                            if (!x || !lu_is_jump(x->op)) continue;
                            int p = lu_label_pos(lmap, lmap_n, x->label_idx);
                            if (p >= H && p <= E) hdr_ok = false;
                        }

                        /* A user label inside the span: a nested function
                         * (compiled AFTER this one's optimization) may
                         * nonlocal-goto it; the OP_NONLOCAL_GOTO resolves
                         * against this function's compiled label indices, so
                         * re-labeling the loop would break it. */
                        ZeroCCFunction *fn = &c->functions[c->lowering.current_func_idx];
                        for (int i = 0; i < fn->nl_label_count && hdr_ok; i++) {
                            int p = lu_label_pos(lmap, lmap_n, fn->nl_label_ir[i]);
                            if (p >= H && p <= E) hdr_ok = false;
                        }
                        /* &&label (OP_LEA_LABEL) of a label located in the
                         * span: the address would differ per copy. */
                        for (int i = 0; i < count && hdr_ok; i++) {
                            Instr *x = ir_get(c, i);
                            if (!x || x->op != OP_LEA_LABEL) continue;
                            int p = lu_label_pos(lmap, lmap_n, x->label_idx);
                            if (p >= H && p <= E) hdr_ok = false;
                        }
                    }

                    /* The loop test: JE/JNE flag, Lexit with
                     * flag = OP_COND_SET(OP_CMP a, b). */
                    if (hdr_ok) {
                        int flag = (int)t->temp_src1;
                        int cs_idx = -1;
                        for (int k = bh_end - 1; k > H; k--) {
                            Instr *x = ir_get(c, k);
                            if (x && x->op == OP_COND_SET && (int)x->temp_dest == flag) {
                                cs_idx = k;
                                break;
                            }
                        }
                        if (cs_idx < 0) {
                            hdr_ok = false;
                        } else {
                            Instr *cs = ir_get(c, cs_idx);
                            int cc = (int)cs->cond;
                            /* CC_FLT_* (CF/ZF setccs) are the UNSIGNED
                             * integer comparison variant (lowering adds 6);
                             * for the small values this pass deals in they
                             * agree with the signed reading. */
                            if (cc >= CC_FLT_LT && cc <= CC_FLT_GE) cc -= 6;
                            if (cc != CC_LT && cc != CC_LE && cc != CC_GT && cc != CC_GE) {
                                hdr_ok = false;
                            } else {
                                if (t->op == OP_JNE) cc = lu_negate_cc(cc);
                                plan->cc = cc;
                            }
                            if (hdr_ok) {
                                int cmp_idx = -1;
                                for (int k = cs_idx - 1; k > H; k--) {
                                    Instr *x = ir_get(c, k);
                                    if (x && x->op == OP_CMP) { cmp_idx = k; break; }
                                }
                                if (cmp_idx < 0) {
                                    hdr_ok = false;
                                } else {
                                    Instr *cm = ir_get(c, cmp_idx);
                                    int64_t v1 = 0, v2 = 0;
                                    int k1 = lu_trace_value(c, H, cmp_idx - 1,
                                                            (int)cm->temp_src1, 0, &v1);
                                    int k2 = lu_trace_value(c, H, cmp_idx - 1,
                                                            (int)cm->temp_src2, 0, &v2);
                                    /* exactly one operand is the constant
                                     * bound; the other is the counter */
                                    if (k1 == LU_VAL_CONST && k2 != LU_VAL_CONST) {
                                        plan->bound = v1;
                                        if (k2 == LU_VAL_SLOT) { plan->counter_kind = 0; plan->counter_slot = v2; }
                                        else if (k2 == LU_VAL_BASE) { plan->counter_kind = 1; plan->counter_temp = (int)v2; }
                                        else hdr_ok = false;
                                    } else if (k2 == LU_VAL_CONST && k1 != LU_VAL_CONST) {
                                        plan->bound = v2;
                                        if (k1 == LU_VAL_SLOT) { plan->counter_kind = 0; plan->counter_slot = v1; }
                                        else if (k1 == LU_VAL_BASE) { plan->counter_kind = 1; plan->counter_temp = (int)v1; }
                                        else hdr_ok = false;
                                    } else {
                                        hdr_ok = false;
                                    }
                                }
                            }
                        }
                    }

                    /* Counter initialization: the last definition before the
                     * header must hold a compile-time constant. */
                    if (hdr_ok) {
                        int64_t init = 0;
                        if (plan->counter_kind == 0) {
                            int64_t S = plan->counter_slot;
                            /* the slot must not be address-taken anywhere
                             * (a pointer store would defeat the counting) */
                            for (int i = 0; i < count; i++) {
                                Instr *x = ir_get(c, i);
                                if (x && x->op == OP_LEA && x->imm64 == S) { hdr_ok = false; break; }
                            }
                            int init_store = -1;
                            if (hdr_ok) {
                                for (int k = H - 1; k >= 0; k--) {
                                    Instr *x = ir_get(c, k);
                                    if (x && x->op == OP_MOV_MEM && x->imm64 == S) {
                                        init_store = k;
                                        break;
                                    }
                                }
                            }
                            if (init_store < 0) {
                                hdr_ok = false;
                            } else if (lu_trace_value(c, 0, init_store - 1,
                                (int)ir_get(c, init_store)->temp_src1, 0, &init) != LU_VAL_CONST) {
                                hdr_ok = false;
                            }
                        } else {
                            int iv = plan->counter_temp;
                            if (lu_trace_value(c, 0, H - 1, iv, 0, &init) != LU_VAL_CONST) {
                                hdr_ok = false;
                            }
                        }
                        if (hdr_ok) plan->init = init;
                    }

                    /* Counter increment: exactly one counter definition in
                     * the span, in the backedge block, equal to +1. */
                    if (hdr_ok) {
                        if (plan->counter_kind == 0) {
                            int64_t S = plan->counter_slot;
                            int store_idx = -1, store_count = 0;
                            for (int i = H; i <= E; i++) {
                                Instr *x = ir_get(c, i);
                                if (x && x->op == OP_MOV_MEM && x->imm64 == S) {
                                    store_count++;
                                    store_idx = i;
                                }
                            }
                            if (store_count != 1 || store_idx < plan->bu_start ||
                                store_idx >= E ||
                                !lu_slot_increment_ok(c, plan->bu_start, store_idx, S)) {
                                hdr_ok = false;
                            }
                        } else {
                            int iv = plan->counter_temp;
                            int def_idx = -1, def_count = 0;
                            for (int i = H; i <= E; i++) {
                                Instr *x = ir_get(c, i);
                                if (x && (int)x->temp_dest == iv) {
                                    def_count++;
                                    def_idx = i;
                                }
                            }
                            if (def_count != 1 || def_idx < plan->bu_start ||
                                def_idx >= E ||
                                !lu_temp_increment_ok(c, plan->bu_start, def_idx, iv)) {
                                hdr_ok = false;
                            }
                        }
                    }

                    /* Trip count.  With a +1 step only the "count up" forms
                     * terminate: continue-while counter < bound (trip =
                     * bound - init) or <= (trip = bound - init + 1).  The
                     * down forms (GT/GE) run forever under +1 and are
                     * rejected. */
                    if (hdr_ok) {
                        int64_t trip = 0;
                        if (plan->cc == CC_LT) trip = plan->bound - plan->init;
                        else if (plan->cc == CC_LE) trip = plan->bound - plan->init + 1;
                        else hdr_ok = false;
                        if (hdr_ok && (trip < 1 || trip > LU_MAX_TRIP)) hdr_ok = false;
                        if (hdr_ok &&
                            (plan->bound < -2147483648LL || plan->bound > 2147483647LL ||
                             plan->init < -2147483648LL || plan->init > 2147483647LL)) {
                            hdr_ok = false;
                        }
                        if (hdr_ok) plan->trip = (int)trip;
                    }
                    ok = hdr_ok;
                }
            }
        }
    }

    free(lmap);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Transform (two-pass rebuild, LICM pattern)                          */
/* ------------------------------------------------------------------ */

static bool lu_apply(ZeroCCCompiler *c, const LUPlan *plan) {
    int count = (int)ir_get_count(c);
    int H = plan->H, E = plan->E, bh_end = plan->bh_end;
    int T = plan->trip;

    LULabel *lmap = NULL;
    int lmap_n = lu_build_label_map(c, &lmap);
    bool ok = false;

    /* In-loop labels (positions strictly inside the span) each get a
     * per-copy clone; each copy header gets a fresh label. */
    int internal_n = 0;
    int *internal_pos = (int *)malloc((size_t)(E - H) * sizeof(int));
    if (!internal_pos) { free(lmap); return false; }
    for (int i = H + 1; i < E; i++) {
        Instr *x = ir_get(c, i);
        if (x && x->op == OP_LABEL) internal_pos[internal_n++] = i;
    }

    int total_new_labels = T + T * internal_n;
    if ((int)c->ir_next_label + total_new_labels + 4 > 60000) {
        free(internal_pos);
        free(lmap);
        return false;
    }

    int *hdr_label = (int *)malloc((size_t)T * sizeof(int));
    int *copy_label = internal_n > 0
        ? (int *)malloc((size_t)T * (size_t)internal_n * sizeof(int)) : NULL;
    if (!hdr_label || (internal_n > 0 && !copy_label)) {
        free(hdr_label);
        free(copy_label);
        free(internal_pos);
        free(lmap);
        return false;
    }
    for (int k = 0; k < T; k++) hdr_label[k] = (int)c->ir_next_label++;
    for (int k = 0; k < T; k++)
        for (int j = 0; j < internal_n; j++)
            copy_label[(size_t)k * (size_t)internal_n + (size_t)j] = (int)c->ir_next_label++;

    int new_total = H + T * (E - bh_end + 1) + (count - E - 1);
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, (size_t)new_total + 16)) {
        goto done;
    }

    int *old_to_new = (int *)malloc((size_t)count * sizeof(int));
    if (!old_to_new) goto done;
    memset(old_to_new, -1, (size_t)count * sizeof(int));

    /* Prefix: [0, H) unchanged. */
    for (int i = 0; i < H; i++) {
        old_to_new[i] = (int)new_arena.count;
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    /* The T copies of [label, body..step, backedge]. */
    for (int k = 0; k < T; k++) {
        Instr lbl;
        memset(&lbl, 0, sizeof(lbl));
        lbl.op = OP_LABEL;
        lbl.label_idx = (uint16_t)hdr_label[k];
        ir_arena_emit(&new_arena, lbl);

        for (int i = bh_end + 1; i < E; i++) {
            Instr x = *ir_get(c, i);
            if (x.op == OP_LABEL) {
                /* In-loop label: give this copy its own clone so mid-body
                 * control flow stays inside one iteration. */
                int j = -1;
                for (int q = 0; q < internal_n; q++) {
                    if (internal_pos[q] == i) { j = q; break; }
                }
                if (j < 0) goto done;
                x.label_idx = (uint16_t)copy_label[(size_t)k * (size_t)internal_n + (size_t)j];
            } else if (lu_is_jump(x.op)) {
                int p = lu_label_pos(lmap, lmap_n, x.label_idx);
                if (p == H) {
                    /* continue (while-form) -> next iteration / exit */
                    x.label_idx = (uint16_t)((k + 1 < T) ? hdr_label[k + 1] : plan->exit_label);
                } else if (p == plan->bu_start && plan->step_label >= 0 && i < plan->bu_start) {
                    /* continue (for-form) -> THIS copy's step label */
                    int j = -1;
                    for (int q = 0; q < internal_n; q++) {
                        if (internal_pos[q] == p) { j = q; break; }
                    }
                    if (j < 0) goto done;
                    x.label_idx = (uint16_t)copy_label[(size_t)k * (size_t)internal_n + (size_t)j];
                } else if (p > H && p < E) {
                    /* any other in-loop label (forward if/else edges) ->
                     * this copy's clone */
                    int j = -1;
                    for (int q = 0; q < internal_n; q++) {
                        if (internal_pos[q] == p) { j = q; break; }
                    }
                    if (j < 0) goto done;
                    x.label_idx = (uint16_t)copy_label[(size_t)k * (size_t)internal_n + (size_t)j];
                }
                /* external targets (break, epilogue, ...) stay put */
            }
            if (old_to_new[i] < 0) old_to_new[i] = (int)new_arena.count;
            ir_arena_emit(&new_arena, x);
        }

        /* Backedge: next copy, or the loop exit for the last copy. */
        Instr jmp;
        memset(&jmp, 0, sizeof(jmp));
        jmp.op = OP_JMP;
        jmp.label_idx = (uint16_t)((k + 1 < T) ? hdr_label[k + 1] : plan->exit_label);
        if (old_to_new[E] < 0) old_to_new[E] = (int)new_arena.count;
        ir_arena_emit(&new_arena, jmp);
    }

    /* Suffix: [E+1, count) unchanged. */
    for (int i = E + 1; i < count; i++) {
        old_to_new[i] = (int)new_arena.count;
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    /* Sprint 135: remap OP_ASM side-table indices to new instruction positions */
    {
        IRAsmArgsArena *asm_arena = &c->ir_asm_args;
        if (asm_arena && asm_arena->table) {
            for (size_t ai = 0; ai < asm_arena->count; ai++) {
                size_t old_pos = asm_arena->table[ai].instr_index;
                if (old_pos < (size_t)count && old_to_new[old_pos] >= 0) {
                    asm_arena->table[ai].instr_index = (size_t)old_to_new[old_pos];
                }
            }
        }
        ir_arena_free(&c->ir_arena);
        c->ir_arena = new_arena;
        ok = true;
    }

done:
    if (!ok) ir_arena_free(&new_arena);
    free(old_to_new);
    free(hdr_label);
    free(copy_label);
    free(internal_pos);
    free(lmap);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Pass entry                                                          */
/* ------------------------------------------------------------------ */

void opt_loop_unroll(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;

    LoopAnalysisState state;
    loop_analysis_init(&state);
    loop_detect(c, &state);

    for (int l = 0; l < state.loop_count; l++) {
        LUPlan plan;
        if (lu_analyze(c, &state.loops[l], &plan)) {
            if (lu_apply(c, &plan)) {
                opt_profile_count_loop_unroll();
                loop_analysis_free(&state);
                return;
            }
        }
    }
    loop_analysis_free(&state);
}
