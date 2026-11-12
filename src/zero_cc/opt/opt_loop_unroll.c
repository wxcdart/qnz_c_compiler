/*
 * Loop full-unrolling pass (Sprint 150, development plan item 3).
 *
 * See opt_loop_unroll.h for the transform contract and gate list.
 */

#include "zero_cc/opt/opt_loop_unroll.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_constant_fold.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/ir_emitter.h"
#include "zero_cc/types/symbols.h"

#define LU_MAX_TRIP  8    /* trip-count cap for full unroll (plan item 3) */
#define LU_MAX_BODY  64   /* max loop span for FULL unroll (x8 worst case) */
#define LU_MAX_PARTIAL_BODY 256 /* max span for partial (x2/x4 copies only) */
#define LU_TRACE_MAX 12   /* value-trace walk depth cap */
/* "Already peeled" marker (Sprint 156, Phase 2): the remainder loop left
 * behind by a partial unroll carries an OP_NOP with this imm in its header
 * block, so a later pass round does not peel the same loop again. */
#define LU_MARKER    0x5EED

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
    int trip;           /* verified constant trip count (full path) */
    /* Phase 2 (Sprint 156): partial unroll of a runtime-bound loop. */
    int partial;        /* 1 = partial path (bound is a runtime value) */
    int factor;         /* unroll factor for the main loop (2 or 4) */
    int bound_kind;     /* partial: 1 = BASE temp, 2 = SLOT */
    int bound_temp;     /* partial: the bound temp (bound_kind == 1) */
    int64_t bound_slot; /* partial: the bound slot (bound_kind == 2) */
    int bound_unsigned; /* partial: test cc was the CF/ZF (unsigned) variant */
    int bound_is_src2;  /* partial: bound operand is the CMP's temp_src2 */
    int cc_le;          /* partial: 1 if the continue condition is LE */
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
/*
 * Resolve temp `t` to the compile-time constant it holds at the point
 * where it is consumed at instruction position `pos` (i.e. from the
 * last definition of `t` strictly before `pos`, walking the
 * value-preserving chain: MOV_REG / SEXT / ZEXT back to a MOV_IMM).
 *
 * Position-awareness is mandatory under the liveness renumber
 * (Sprint 151b): a dead id is legally reused, so the same id can be
 * defined by different instructions at different points in the
 * function.  The old whole-function "was this id ever defined by
 * MOV_IMM?" scan resolved the loop counter to the constant 0 of an
 * unrelated local that shared the compacted id, which made the
 * counter-trace report "both operands constant" and rejected every
 * for-loop (the test-loop-unroll gate went red after 151b and sat
 * unwatched because it is outside the standard battery).
 */
static int lu_resolve_const(ZeroCCCompiler *c, int pos, int t, int64_t *val_out) {
    if (t == TEMP_NONE) return 0;
    if (ir_is_constant(t)) { *val_out = ir_get_constant(t); return 1; }
    int depth = 0;
    while (pos > 0 && depth <= LU_TRACE_MAX) {
        int k = -1;
        for (int j = pos - 1; j >= 0; j--) {
            Instr *d = ir_get(c, j);
            if (d && (int)d->temp_dest == t) { k = j; break; }
        }
        if (k < 0) return 0;  /* id not defined before pos */
        Instr *d = ir_get(c, k);
        switch (d->op) {
            case OP_MOV_IMM:
                *val_out = d->imm64;
                return 1;
            case OP_MOV_REG:
                pos = k; t = (int)d->temp_src1; depth++; break;
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                pos = k; t = (int)d->temp_src1; depth++; break;
            default:
                return 0;  /* non-constant def (stack load, binop, ...) */
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
    if (lu_resolve_const(c, hi + 1, temp, &cv)) { *val_out = cv; return LU_VAL_CONST; }
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
 * Backward value trace: from the value stored at store_idx (temp `t`),
 * follow defining instructions backward (strictly decreasing positions)
 * through copies / in-place extends / "+1" steps until reaching a LOAD
 * from slot S.  Returns the load position if it sits inside the loop
 * ([H, store_idx)) - i.e. it reloads the counter each iteration.
 *
 * Unlike a forward scan of the backedge block, this tolerates CSE'd
 * counter reloads: an earlier pass may have deleted the backedge's own
 * load of S when the body already loaded S (e.g. `s += i`), leaving
 * backedge = copy-of-body-load, +1, store.  Body ops that merely USE
 * the counter value are irrelevant - only the store's own value chain
 * matters, and the in-span store count is separately pinned to 1.
 */
static bool lu_trace_counter_load(ZeroCCCompiler *c, int H, int store_idx,
                                  int t, int64_t S, int *load_pos_out) {
    int bound = store_idx;
    for (int guard = 0; guard < 64; guard++) {
        int def = -1;
        for (int i = bound - 1; i >= 0; i--) {
            Instr *x = ir_get(c, i);
            if (x && (int)x->temp_dest == t) { def = i; break; }
        }
        if (def < 0) return false;
        Instr *x = ir_get(c, def);
        int s1 = (int)x->temp_src1, s2 = (int)x->temp_src2, d = (int)x->temp_dest;
        int64_t cv;
        bound = def;  /* next search is strictly earlier - progress */
        switch (x->op) {
            case OP_MOV:  /* load from slot */
                if (x->imm64 == S && s1 == TEMP_NONE && def >= H) {
                    *load_pos_out = def;
                    return true;
                }
                return false;
            case OP_MOV_REG:
                if (s1 == t) continue;              /* in-place no-op */
                if (s1 != TEMP_NONE) { t = s1; continue; }
                return false;
            case OP_ADD:
                /* def has d == t: t = X + 1 or 1 + X (or t = t + 1) */
                if (s2 != t && lu_resolve_const(c, def, s2, &cv) && cv == 1) { t = s1; continue; }
                if (s1 != t && lu_resolve_const(c, def, s1, &cv) && cv == 1) { t = s2; continue; }
                return false;
            case OP_ADD_IMM:
                if ((int)x->imm != 1) return false;
                if (s1 == t) continue;              /* in-place t += 1 */
                if (s1 != TEMP_NONE) { t = s1; continue; }  /* t = s1 + 1 */
                return false;
            case OP_INC:
                if (d == t && (s1 == t || s1 == TEMP_NONE)) continue;
                return false;
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (s1 == t) continue;              /* in-place extend */
                if (s1 != TEMP_NONE) { t = s1; continue; }
                return false;
            default:
                return false;
        }
    }
    return false;
}

/*
 * Slot-form increment: the single in-span store of counter slot S (at
 * store_idx, in the backedge block) must write (value loaded from S this
 * iteration) + 1.  The per-iteration load may sit anywhere in the loop
 * (CSE can merge it with the body's own counter load); see
 * lu_trace_counter_load.  Independent instructions are allowed.
 */
static bool lu_slot_increment_ok(ZeroCCCompiler *c, int H, int bu_start,
                                 int store_idx, int64_t S) {
    (void)bu_start;
    Instr *st = ir_get(c, store_idx);
    if (!st || st->op != OP_MOV_MEM || st->imm64 != S ||
        st->temp_src1 == TEMP_NONE) return false;
    int load_pos = -1;
    return lu_trace_counter_load(c, H, store_idx, (int)st->temp_src1, S, &load_pos);
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
            if (s1 == iv && s2 != iv && lu_resolve_const(c, def_idx, s2, &cv) && cv == 1) return true;
            if (s2 == iv && s1 != iv && lu_resolve_const(c, def_idx, s1, &cv) && cv == 1) return true;
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
                    (((y1 == iv && y2 != iv && lu_resolve_const(c, k, y2, &cv) && cv == 1) ||
                     (y2 == iv && y1 != iv && lu_resolve_const(c, k, y1, &cv) && cv == 1)));
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

/*
 * Phase 2 re-entry soundness: a partially-unrolled loop must see a
 * CONSTANT counter at EVERY entry, because the prologue's main-loop
 * bound b2 is derived from that init and the counter is advanced by the
 * loop's own backedge.  A loop whose span is strictly enclosed by another
 * detected loop is RE-ENTERED on each enclosing iteration: the counter
 * (incremented by this loop's backedge) carries its previous run's final
 * value into the next entry, so the "last definition before the header"
 * init is stale after the first pass (c-torture execute/20041213-2: a
 * nested `for (n = next_n; j < n; j++)` whose counter j is re-entered
 * carrying j from the prior outer trip).  Top-level loops have a single
 * entry, where the init IS the entry value.  Reject any loop enclosed by
 * another (conservative: also declines a safe nested loop whose counter
 * is re-initialized each entry - such loops are usually constant-trip and
 * served by full unroll).
 */
static bool lu_is_enclosed(const LoopAnalysisState *state, int H, int E) {
    for (int m = 0; m < state->loop_count; m++) {
        int h2 = state->loops[m].header_idx;
        int e2 = state->loops[m].exit_idx;
        if (h2 < H && e2 > E) return true;
    }
    return false;
}

static bool lu_analyze(ZeroCCCompiler *c, const LoopAnalysisState *state,
                       LoopInfo *loop, LUPlan *plan) {
    int count = (int)ir_get_count(c);
    int H = loop->header_idx;
    int E = loop->exit_idx;
    if (H < 0 || E < 0 || E >= count || E < H) return false;
    if (E - H + 1 > LU_MAX_PARTIAL_BODY) return false;

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
                            /* The header def is dead - and therefore fine -
                             * once its id is redefined past the test; only a
                             * use reaching the id BEFORE any redefine would
                             * read the once-executed header value.  The
                             * liveness renumber (Sprint 151b) legally reuses
                             * a dead header temp's id for a body def, so the
                             * scan stops at a redefine (Sprint 150 gate:
                             * opt_temp_renumber compaction used to make this
                             * check reject every loop).  A temp_dest on
                             * OP_CALL/OP_SYSCALL/OP_CLONE is an arg count,
                             * not a redefine. */
                            for (int j = bh_end + 1; j < count; j++) {
                                Instr *u = ir_get(c, j);
                                if (!u) continue;
                                if (u->op != OP_CALL && u->op != OP_SYSCALL &&
                                    u->op != OP_CLONE &&
                                    (int)u->temp_dest == d) {
                                    break;  /* redefined: header def dead */
                                }
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
                            if (p >= H && p <= E) {
                                hdr_ok = false;
                            }
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
                            int cc_raw = cc;
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
                                    } else if (k1 != LU_VAL_CONST && k2 != LU_VAL_CONST &&
                                                 !lu_is_enclosed(state, H, E)) {
                                        /* Phase 2 (Sprint 156): runtime bound -
                                         * partial unroll + remainder peel.
                                         * RE-ENTRY SOUNDNESS: the prologue
                                         * derives b2 from a CONSTANT counter
                                         * init, valid only if the loop is
                                         * entered once; a loop enclosed by
                                         * another is re-entered each outer
                                         * trip, carrying the counter's prior
                                         * final value (lu_is_enclosed), so
                                         * such loops fall through to the
                                         * reject branch below.
                                         * The COUNTER is the operand the span
                                         * writes (its +1 increment); the BOUND
                                         * is the other, which must be a value
                                         * invariant across the loop: a stack
                                         * slot (never stored / address-taken in
                                         * the span) or a temp defined before
                                         * the header and never redefined in it.
                                         * The counter keeps the narrow [H, cmp)
                                         * window (its value at test time comes
                                         * from the header load); the bound is
                                         * traced over the whole prefix because
                                         * its defining load may sit before H. */
                                        int64_t w1 = 0, w2 = 0;
                                        int kw1 = lu_trace_value(c, 0, cmp_idx - 1,
                                                                 (int)cm->temp_src1, 0, &w1);
                                        int kw2 = lu_trace_value(c, 0, cmp_idx - 1,
                                                                 (int)cm->temp_src2, 0, &w2);
                                        enum { CND_NONE = 0, CND_SLOT, CND_TEMP };
                                        int cand1 = (k1 == LU_VAL_SLOT) ? CND_SLOT
                                                     : (k1 == LU_VAL_BASE) ? CND_TEMP : CND_NONE;
                                        int cand2 = (k2 == LU_VAL_SLOT) ? CND_SLOT
                                                     : (k2 == LU_VAL_BASE) ? CND_TEMP : CND_NONE;
                                        int inc1 = 0, inc2 = 0;
                                        if (cand1 == CND_SLOT)
                                            for (int i = H; i <= E; i++) {
                                                Instr *x = ir_get(c, i);
                                                if (x && x->op == OP_MOV_MEM && x->imm64 == v1) inc1 = 1;
                                            }
                                        else if (cand1 == CND_TEMP)
                                            for (int i = H; i <= E; i++) {
                                                Instr *x = ir_get(c, i);
                                                if (x && (int)x->temp_dest == (int)cm->temp_src1) inc1 = 1;
                                            }
                                        if (cand2 == CND_SLOT)
                                            for (int i = H; i <= E; i++) {
                                                Instr *x = ir_get(c, i);
                                                if (x && x->op == OP_MOV_MEM && x->imm64 == v2) inc2 = 1;
                                            }
                                        else if (cand2 == CND_TEMP)
                                            for (int i = H; i <= E; i++) {
                                                Instr *x = ir_get(c, i);
                                                if (x && (int)x->temp_dest == (int)cm->temp_src2) inc2 = 1;
                                            }
                                        if (inc1 && !inc2) {
                                            plan->counter_kind = (cand1 == CND_SLOT) ? 0 : 1;
                                            if (cand1 == CND_SLOT) plan->counter_slot = v1;
                                            else plan->counter_temp = (int)cm->temp_src1;
                                            plan->bound_is_src2 = 1;
                                            if (kw2 == LU_VAL_SLOT) { plan->bound_kind = 2; plan->bound_slot = w2; }
                                            else if (kw2 == LU_VAL_BASE) { plan->bound_kind = 1; plan->bound_temp = (int)cm->temp_src2; }
                                            else hdr_ok = false;
                                        } else if (inc2 && !inc1) {
                                            plan->counter_kind = (cand2 == CND_SLOT) ? 0 : 1;
                                            if (cand2 == CND_SLOT) plan->counter_slot = v2;
                                            else plan->counter_temp = (int)cm->temp_src2;
                                            plan->bound_is_src2 = 0;
                                            if (kw1 == LU_VAL_SLOT) { plan->bound_kind = 2; plan->bound_slot = w1; }
                                            else if (kw1 == LU_VAL_BASE) { plan->bound_kind = 1; plan->bound_temp = (int)cm->temp_src1; }
                                            else hdr_ok = false;
                                        } else {
                                            hdr_ok = false;
                                        }
                                        if (hdr_ok && plan->bound_kind != 0) {
                                            plan->partial = 1;
                                            plan->bound_unsigned =
                                                (cc_raw >= CC_FLT_LT && cc_raw <= CC_FLT_GE);
                                            plan->cc_le = (plan->cc == CC_LE);
                                            /* count-up only (LT/LE): the proof
                                             * that the main-loop interior tests
                                             * are always true needs b2 <= bound.
                                             * (The full path rejects GT/GE in
                                             * the trip-count section; this
                                             * path needs it here.) */
                                            if (plan->cc != CC_LT && plan->cc != CC_LE)
                                                hdr_ok = false;
                                            plan->factor = (E - bh_end >= 16) ? 4 : 2;
                                            if (hdr_ok && plan->bound_kind == 1) {
                                                int bt = plan->bound_temp;
                                                for (int i = H + 1; i < cmp_idx && hdr_ok; i++) {
                                                    Instr *x = ir_get(c, i);
                                                    if (x && (int)x->temp_dest == bt)
                                                        hdr_ok = false;  /* redefined per iteration */
                                                }
                                                for (int i = H + 1; i < E && hdr_ok; i++) {
                                                    Instr *x = ir_get(c, i);
                                                    if (x && (int)x->temp_dest == bt)
                                                        hdr_ok = false;  /* redefined in the span */
                                                }
                                                int found = 0;
                                                for (int i = H - 1; i >= 0 && !found; i--) {
                                                    Instr *x = ir_get(c, i);
                                                    if (x && (int)x->temp_dest == bt) found = 1;
                                                }
                                                if (!found) hdr_ok = false;  /* no entry value */
                                            } else if (hdr_ok && plan->bound_kind == 2) {
                                                int64_t bs = plan->bound_slot;
                                                for (int i = H + 1; i < E && hdr_ok; i++) {
                                                    Instr *x = ir_get(c, i);
                                                    if (x && x->op == OP_MOV_MEM && x->imm64 == bs)
                                                        hdr_ok = false;  /* stored in the span */
                                                }
                                                for (int i = 0; i < count && hdr_ok; i++) {
                                                    Instr *x = ir_get(c, i);
                                                    if (x && x->op == OP_LEA && x->imm64 == bs)
                                                        hdr_ok = false;  /* address taken */
                                                }
                                            }
                                            /* Re-peel guard: an already-peeled
                                             * remainder loop carries the marker
                                             * NOP in its header block. */
                                            for (int i = H + 1; i < bh_end && hdr_ok; i++) {
                                                Instr *x = ir_get(c, i);
                                                if (x && x->op == OP_NOP && (int)x->imm == LU_MARKER)
                                                    hdr_ok = false;
                                            }
                                        }

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
                                !lu_slot_increment_ok(c, H, plan->bu_start, store_idx, S)) {
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

                    /* Trip count (full path only).  With a +1 step only the
                     * "count up" forms terminate: continue-while counter <
                     * bound (trip = bound - init) or <= (trip = bound - init
                     * + 1).  The down forms (GT/GE) run forever under +1 and
                     * are rejected.  The partial path (runtime bound) has no
                     * compile-time trip count; its count-up requirement is
                     * enforced in the operand section. */
                    if (hdr_ok && !plan->partial) {
                        int64_t trip = 0;
                        if (plan->cc == CC_LT) trip = plan->bound - plan->init;
                        else if (plan->cc == CC_LE) trip = plan->bound - plan->init + 1;
                        else hdr_ok = false;
                        if (hdr_ok && (trip < 1 || trip > LU_MAX_TRIP)) hdr_ok = false;
                        if (hdr_ok && (E - H + 1) > LU_MAX_BODY) hdr_ok = false;  /* full unroll cap */
                        if (hdr_ok &&
                            (plan->bound < -2147483648LL || plan->bound > 2147483647LL ||
                             plan->init < -2147483648LL || plan->init > 2147483647LL)) {
                            hdr_ok = false;
                        }
                        if (hdr_ok) plan->trip = (int)trip;
                    }
                    ok = hdr_ok;                }
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

    /* Sprint 154: the OP_ASM payload travels with its anchor in the
     * buffer - nothing to remap. */
    {
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
/* Phase 2 transform: partial unroll + remainder peel (Sprint 156)     */
/* ------------------------------------------------------------------ */

/*
 * Runtime-bound partial unroll.
 *
 *   for (i = init; i <(=) N; i++) body        (N a runtime value)
 *
 * becomes (factor f, b2 = init + f*trunc((N - init + [cc==LE]) / f)):
 *
 *   b2 = <prologue, overflow-free, see below>
 *   Lmain:
 *     <header test vs b2, same cc/JE-JNE form>   -> H (fail => remainder)
 *     [f verbatim copies of (body, step)]         (the counter step is in
 *                                                  each copy: f original
 *                                                  iterations per main-loop
 *                                                  iteration)
 *     JMP Lmain
 *   Lrem:  (== the ORIGINAL loop, verbatim, marker NOP in the header)
 *     [label H, test vs N, body, step, backedge]  (runs N - b2 < f times)
 *   Lexit:
 *
 * Exactness: the copies are bit-for-bit the original body+step, so no
 * affine re-substitution is needed; the main loop's interior iterations
 * ARE the original iterations.  The only tests skipped between copies
 * are provably true: inside the main loop the counter stays < b2 <= N
 * (LT) / < b2 <= N + 1 (LE), so the original test counter <(=) N would
 * pass.  Breaks jump to Lexit (past the remainder); continues re-aim at
 * the next copy's start (to-H edges) or the copy's own step label
 * (for-form); in-loop labels get per-copy clones as in the full unroll.
 * The remainder loop carries the LU_MARKER NOP so a later pass round
 * does not peel it again; the main loop re-qualifies as nothing (f
 * counter increments in its span) or sits over LU_MAX_BODY.
 *
 * Prologue (init is a compile-time constant; k = (cc == CC_LE);
 * init' = init - k so the trip count is (N - init')):
 *   unsigned:  d  = N - init'            (wrap-defined)
 *              b2 = init + (d & ~(f-1))  (wrap-defined; b2 <= N)
 *   signed:    r  = (N % f) - init'%f, corrected into (-f, f)
 *              b2 = N - r, then +1 if k.  N - r cannot overflow: r's
 *              sign follows (N - init')'s and init' is finite; the +1
 *              overflows only where the ORIGINAL loop's final counter
 *              increment already overflows (pre-existing UB).
 *   No prologue op is width-marked, so zero_san emits no new traps, and
 *   every op is defined wherever the original loop was defined: the
 *   transform introduces no new UB.
 */

static void lu_emit_label(IRArena *a, int label) {
    Instr x; memset(&x, 0, sizeof(x));
    x.op = OP_LABEL; x.label_idx = (uint16_t)label;
    ir_arena_emit(a, x);
}

static void lu_emit_jump(IRArena *a, uint8_t op, int flag, int label) {
    /* flag is the branch-condition temp for JE/JNE/JCOND; pass TEMP_NONE
     * for the unconditional OP_JMP. */
    Instr x; memset(&x, 0, sizeof(x));
    x.op = op; x.temp_src1 = (uint32_t)(flag < 0 ? TEMP_NONE : flag);
    x.label_idx = (uint16_t)label;
    ir_arena_emit(a, x);
}

static void lu_emit_mov_imm(IRArena *a, int d, int64_t v) {
    Instr x; memset(&x, 0, sizeof(x));
    x.op = OP_MOV_IMM; x.temp_dest = (uint32_t)d;
    x.imm = (int16_t)v; x.imm64 = v;
    ir_arena_emit(a, x);
}

static void lu_emit_binop(IRArena *a, uint8_t op, int d, int s1, int s2) {
    Instr x; memset(&x, 0, sizeof(x));
    x.op = op; x.temp_dest = (uint32_t)d;
    x.temp_src1 = (uint32_t)s1; x.temp_src2 = (uint32_t)s2;
    ir_arena_emit(a, x);
}

static void lu_emit_imm_alu(IRArena *a, uint8_t op, int d, int64_t imm) {
    /* in-place dest = dest +/- imm (OP_ADD_IMM / OP_SUB_IMM) */
    Instr x; memset(&x, 0, sizeof(x));
    x.op = op; x.temp_dest = (uint32_t)d; x.imm = (int16_t)imm;
    ir_arena_emit(a, x);
}

static void lu_emit_cmp_set(IRArena *a, int s1, int s2, int flag, int cc) {
    Instr x; memset(&x, 0, sizeof(x));
    x.op = OP_CMP; x.temp_src1 = (uint32_t)s1; x.temp_src2 = (uint32_t)s2;
    ir_arena_emit(a, x);
    x.op = OP_COND_SET; x.temp_dest = (uint32_t)flag; x.cond = (uint8_t)cc;
    ir_arena_emit(a, x);
}

/*
 * For a SLOT bound: walk back from the CMP operand through in-place
 * extends / MOV_REGs to the OP_MOV slot load, recording the in-place
 * extend to replicate after the prologue load.  Returns 1 on success.
 */
static int lu_bound_load_chain(ZeroCCCompiler *c, int cmp_idx, int operand,
                               int64_t expect_slot, int *ext_out) {
    *ext_out = 0;
    int t = operand;
    int hi = cmp_idx - 1;
    for (int depth = 0; depth <= LU_TRACE_MAX; depth++) {
        int k = -1;
        for (int j = hi; j >= 0; j--) {
            Instr *d = ir_get(c, j);
            if (d && (int)d->temp_dest == t) { k = j; break; }
        }
        if (k < 0) return 0;
        Instr *d = ir_get(c, k);
        switch (d->op) {
            case OP_MOV:
                return (d->temp_src1 == TEMP_NONE && d->imm64 == expect_slot);
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                if (d->temp_src1 != d->temp_dest) return 0;
                if (*ext_out != 0 && *ext_out != (int)d->op) return 0;
                *ext_out = (int)d->op;
                hi = k - 1;
                break;
            case OP_MOV_REG:
                t = (int)d->temp_src1;
                hi = k - 1;
                break;
            default:
                return 0;
        }
    }
    return 0;
}

static bool lu_apply_partial(ZeroCCCompiler *c, const LUPlan *plan) {
    int count = (int)ir_get_count(c);
    int H = plan->H, E = plan->E, bh_end = plan->bh_end;
    int f = plan->factor;
    int64_t init = plan->init;
    int k = plan->cc_le;              /* LE adds one iteration */
    int64_t init_p = init - k;        /* trip = N - init' */

    LULabel *lmap = NULL;
    int lmap_n = lu_build_label_map(c, &lmap);

    /* Re-locate the test (jump at bh_end <- COND_SET <- CMP). */
    Instr *tj = ir_get(c, bh_end);
    Instr *hl = ir_get(c, H);
    if (!tj || !hl || hl->op != OP_LABEL) { free(lmap); return false; }
    int H_label = (int)hl->label_idx;  /* the original header label */
    int flag = (int)tj->temp_src1;
    int cs_idx = -1, cmp_idx = -1;
    for (int kk = bh_end - 1; kk > H; kk--) {
        Instr *x = ir_get(c, kk);
        if (x && x->op == OP_COND_SET && (int)x->temp_dest == flag) { cs_idx = kk; break; }
    }
    if (cs_idx < 0) { free(lmap); return false; }
    for (int kk = cs_idx - 1; kk > H; kk--) {
        Instr *x = ir_get(c, kk);
        if (x && x->op == OP_CMP) { cmp_idx = kk; break; }
    }
    if (cmp_idx < 0) { free(lmap); return false; }
    Instr *cm = ir_get(c, cmp_idx);
    int bound_opnd = plan->bound_is_src2 ? (int)cm->temp_src2 : (int)cm->temp_src1;

    /* In-loop labels (positions strictly inside the span) each get a
     * per-copy clone. */
    int internal_n = 0;
    int *internal_pos = (int *)malloc((size_t)(E - H) * sizeof(int));
    int *copy_start = (int *)malloc((size_t)f * sizeof(int));
    int *copy_label = NULL;
    if (!internal_pos || !copy_start) {
        free(internal_pos); free(copy_start); free(lmap);
        return false;
    }
    for (int i = H + 1; i < E; i++) {
        Instr *x = ir_get(c, i);
        if (x && x->op == OP_LABEL) internal_pos[internal_n++] = i;
    }
    if (internal_n > 0) {
        copy_label = (int *)malloc((size_t)f * (size_t)internal_n * sizeof(int));
        if (!copy_label) {
            free(internal_pos); free(copy_start); free(lmap);
            return false;
        }
    }
    int step_j = -1;
    for (int q = 0; q < internal_n; q++)
        if (internal_pos[q] == plan->bu_start) step_j = q;

    /* Labels: Lmain, f copy starts, f x internal clones,
     * 4 prologue labels for the signed general path. */
    int total_new = 1 + f + f * internal_n + 8;
    if ((int)c->ir_next_label + total_new > 60000) {
        free(internal_pos); free(copy_start); free(copy_label); free(lmap);
        return false;
    }
    int Lmain = (int)c->ir_next_label++;
    for (int kk = 0; kk < f; kk++) copy_start[kk] = (int)c->ir_next_label++;
    for (int kk = 0; kk < f; kk++)
        for (int j = 0; j < internal_n; j++)
            copy_label[(size_t)kk * (size_t)internal_n + (size_t)j] = (int)c->ir_next_label++;
    int Lpos = (int)c->ir_next_label++, Lm1 = (int)c->ir_next_label++;
    int Lm2 = (int)c->ir_next_label++, Lm3 = (int)c->ir_next_label++;
    int Lm4 = (int)c->ir_next_label++, Lm5 = (int)c->ir_next_label++;
    int Lp1 = (int)c->ir_next_label++, Lp2 = (int)c->ir_next_label++;
    int Lp3 = (int)c->ir_next_label++, Lp4 = (int)c->ir_next_label++;
    int Lp5 = (int)c->ir_next_label++, Ldone = (int)c->ir_next_label++;

    int new_total = H
        + 48                       /* prologue budget (signed path) */
        + (bh_end - H + 1)          /* main header (purity + swapped test) */
        + f * (E - bh_end)          /* f copies: start label + (body..step) */
        + 1                          /* final JMP Lmain */
        + (E - H + 2)                /* remainder: label + marker + span */
        + (count - E - 1)            /* suffix */
        + 16;
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, (size_t)new_total)) {
        free(internal_pos); free(copy_start); free(copy_label); free(lmap);
        return false;
    }
    bool ok = false;

    /* Prefix [0, H) verbatim. */
    for (int i = 0; i < H; i++)
        ir_arena_emit(&new_arena, *ir_get(c, i));

    /* ---- Prologue: compute the main-loop bound t_b2 ---- */
    int t_bv = plan->bound_kind == 1 ? plan->bound_temp : -1;
    if (plan->bound_kind == 2) {
        int ext = 0;
        if (!lu_bound_load_chain(c, cmp_idx, bound_opnd, plan->bound_slot, &ext))
            goto fail;
        t_bv = zc_alloc_temp(c);
        Instr ld; memset(&ld, 0, sizeof(ld));
        ld.op = OP_MOV; ld.temp_dest = (uint32_t)t_bv;
        ld.temp_src1 = TEMP_NONE; ld.imm64 = plan->bound_slot;
        ir_arena_emit(&new_arena, ld);
        if (ext != 0) {
            Instr ex; memset(&ex, 0, sizeof(ex));
            ex.op = (uint8_t)ext; ex.temp_dest = (uint32_t)t_bv;
            ex.temp_src1 = (uint32_t)t_bv;
            ir_arena_emit(&new_arena, ex);
        }
    }
    int t_b2 = -1;
    if (plan->bound_unsigned) {
        /* b2 = init + ((N - init') & ~(f-1))  (all wrap-defined) */
        int t_d = t_bv;
        if (init_p != 0) {
            int t_init = zc_alloc_temp(c);
            lu_emit_mov_imm(&new_arena, t_init, init_p);
            t_d = zc_alloc_temp(c);
            lu_emit_binop(&new_arena, OP_SUB, t_d, t_bv, t_init);
        }
        int t_mask = zc_alloc_temp(c);
        lu_emit_mov_imm(&new_arena, t_mask, -(int64_t)f);
        int t_q = zc_alloc_temp(c);
        lu_emit_binop(&new_arena, OP_AND, t_q, t_d, t_mask);
        t_b2 = (init == 0) ? t_q : zc_alloc_temp(c);
        if (init != 0) {
            int t_init2 = zc_alloc_temp(c);
            lu_emit_mov_imm(&new_arena, t_init2, init);
            lu_emit_binop(&new_arena, OP_ADD, t_b2, t_q, t_init2);
        }
    } else {
        /* signed: b2 = N - r [+ k] with r = (N - init_p) % f (the C
         * remainder).  r0 = (N % f) - init_p%f is congruent to
         * d = N - init_p (mod f) and lies in (-2f, 2f) WITHOUT ever
         * forming d (N - init_p can overflow for 64-bit bounds); fold
         * r0 into [0, f) when d > 0 and into (-f, 0] when d <= 0
         * (d == 0 yields r0 == 0 automatically).  N - r cannot
         * overflow: r has d's sign, |r| < f, and init_p is finite;
         * the final +1 (LE) overflows only where the original loop's
         * last counter increment already overflows (pre-existing UB). */
        int t_f = zc_alloc_temp(c);
        lu_emit_mov_imm(&new_arena, t_f, f);
        int t_nf = zc_alloc_temp(c);
        lu_emit_mov_imm(&new_arena, t_nf, -(int64_t)f);
        int t_zero = zc_alloc_temp(c);
        lu_emit_mov_imm(&new_arena, t_zero, 0);
        int t_ra = zc_alloc_temp(c);
        lu_emit_binop(&new_arena, OP_MOD, t_ra, t_bv, t_f);
        int t_r0 = t_ra;
        if (init_p % (int64_t)f != 0) {
            int t_r = zc_alloc_temp(c);
            lu_emit_mov_imm(&new_arena, t_r, init_p % (int64_t)f);
            t_r0 = zc_alloc_temp(c);
            lu_emit_binop(&new_arena, OP_SUB, t_r0, t_ra, t_r);
        }
        int t_ip = zc_alloc_temp(c);
        lu_emit_mov_imm(&new_arena, t_ip, init_p);
        int sf = zc_alloc_temp(c), f1 = zc_alloc_temp(c), f2 = zc_alloc_temp(c);
        lu_emit_cmp_set(&new_arena, t_bv, t_ip, sf, CC_GT);  /* d > 0? */
        lu_emit_jump(&new_arena, OP_JNE, sf, Lpos);
        /* ---- d <= 0: fold r0 into (-f, 0] ---- */
        lu_emit_cmp_set(&new_arena, t_r0, t_f, f1, CC_GT);
        lu_emit_jump(&new_arena, OP_JNE, f1, Lm1);
        lu_emit_jump(&new_arena, OP_JMP, -1, Lm2);
        lu_emit_label(&new_arena, Lm1);
        lu_emit_imm_alu(&new_arena, OP_SUB_IMM, t_r0, f);
        lu_emit_label(&new_arena, Lm2);
        lu_emit_cmp_set(&new_arena, t_r0, t_f, f1, CC_GT);
        lu_emit_jump(&new_arena, OP_JNE, f1, Lm3);
        lu_emit_jump(&new_arena, OP_JMP, -1, Lm4);
        lu_emit_label(&new_arena, Lm3);
        lu_emit_imm_alu(&new_arena, OP_SUB_IMM, t_r0, f);
        lu_emit_label(&new_arena, Lm4);
        lu_emit_cmp_set(&new_arena, t_r0, t_nf, f2, CC_LE);
        lu_emit_jump(&new_arena, OP_JNE, f2, Lm5);
        lu_emit_jump(&new_arena, OP_JMP, -1, Ldone);
        lu_emit_label(&new_arena, Lm5);
        lu_emit_imm_alu(&new_arena, OP_ADD_IMM, t_r0, f);
        lu_emit_jump(&new_arena, OP_JMP, -1, Ldone);
        lu_emit_label(&new_arena, Lpos);
        /* ---- d > 0: fold r0 into [0, f) ---- */
        lu_emit_cmp_set(&new_arena, t_r0, t_f, f1, CC_GE);
        lu_emit_jump(&new_arena, OP_JNE, f1, Lp1);
        lu_emit_jump(&new_arena, OP_JMP, -1, Lp2);
        lu_emit_label(&new_arena, Lp1);
        lu_emit_imm_alu(&new_arena, OP_SUB_IMM, t_r0, f);
        lu_emit_label(&new_arena, Lp2);
        lu_emit_cmp_set(&new_arena, t_r0, t_zero, f2, CC_LT);
        lu_emit_jump(&new_arena, OP_JNE, f2, Lp3);
        lu_emit_jump(&new_arena, OP_JMP, -1, Lp4);
        lu_emit_label(&new_arena, Lp3);
        lu_emit_imm_alu(&new_arena, OP_ADD_IMM, t_r0, f);
        lu_emit_label(&new_arena, Lp4);
        lu_emit_cmp_set(&new_arena, t_r0, t_zero, f2, CC_LT);
        lu_emit_jump(&new_arena, OP_JNE, f2, Lp5);
        lu_emit_jump(&new_arena, OP_JMP, -1, Ldone);
        lu_emit_label(&new_arena, Lp5);
        lu_emit_imm_alu(&new_arena, OP_ADD_IMM, t_r0, f);
        lu_emit_label(&new_arena, Ldone);
        /* b2 = N - r for BOTH cc forms: the LE extra iteration is already
         * inside r (via init_p = init - k), and the main test keeps the
         * original cc (i <= b2 covers init..b2 = init + f*q values). */
        t_b2 = zc_alloc_temp(c);
        lu_emit_binop(&new_arena, OP_SUB, t_b2, t_bv, t_r0);
    }

    /* ---- Main loop header: label, purity block, test vs t_b2 ---- */
    lu_emit_label(&new_arena, Lmain);
    for (int i = H + 1; i < bh_end; i++) {
        Instr x = *ir_get(c, i);
        if (i == cmp_idx) {
            if (plan->bound_is_src2) x.temp_src2 = (uint32_t)t_b2;
            else x.temp_src1 = (uint32_t)t_b2;
        }
        ir_arena_emit(&new_arena, x);
    }
    {
        Instr x = *tj;                 /* JE/JNE, re-aimed at the remainder */
        x.label_idx = (uint16_t)H_label;
        ir_arena_emit(&new_arena, x);
    }

    /* ---- f verbatim copies of (body..step) ---- */
    for (int kk = 0; kk < f; kk++) {
        lu_emit_label(&new_arena, copy_start[kk]);
        for (int i = bh_end + 1; i < E; i++) {
            Instr x = *ir_get(c, i);
            if (x.op == OP_LABEL) {
                int j = -1;
                for (int q = 0; q < internal_n; q++)
                    if (internal_pos[q] == i) { j = q; break; }
                if (j < 0) goto fail;
                x.label_idx = (uint16_t)copy_label[(size_t)kk * (size_t)internal_n + (size_t)j];
            } else if (lu_is_jump(x.op)) {
                int p = lu_label_pos(lmap, lmap_n, x.label_idx);
                if (p == H) {
                    /* continue / step-tail: the next ORIGINAL iteration is
                     * copy kk+1 (the skipped test is provably true inside
                     * the main loop); the last copy re-enters the test. */
                    x.label_idx = (uint16_t)((kk + 1 < f) ? copy_start[kk + 1] : Lmain);
                } else if (p == plan->bu_start && plan->step_label >= 0 &&
                           i < plan->bu_start && step_j >= 0) {
                    /* for-form continue: THIS copy's own step label. */
                    x.label_idx = (uint16_t)copy_label[(size_t)kk * (size_t)internal_n + (size_t)step_j];
                } else if (p > H && p < E) {
                    /* forward in-loop edge (if/else): this copy's clone. */
                    int j = -1;
                    for (int q = 0; q < internal_n; q++)
                        if (internal_pos[q] == p) { j = q; break; }
                    if (j < 0) goto fail;
                    x.label_idx = (uint16_t)copy_label[(size_t)kk * (size_t)internal_n + (size_t)j];
                }
                /* external targets (break -> exit label, ...) stay put */
            }
            ir_arena_emit(&new_arena, x);
        }
    }
    lu_emit_jump(&new_arena, OP_JMP, -1, Lmain);

    /* ---- Remainder loop: the original [H, E] verbatim + marker ---- */
    lu_emit_label(&new_arena, H_label);
    {
        Instr mk; memset(&mk, 0, sizeof(mk));
        mk.op = OP_NOP; mk.temp_dest = TEMP_NONE;
        mk.imm = (int16_t)LU_MARKER; mk.imm64 = LU_MARKER;
        ir_arena_emit(&new_arena, mk);
    }
    for (int i = H + 1; i <= E; i++)
        ir_arena_emit(&new_arena, *ir_get(c, i));

    /* ---- Suffix [E+1, count) verbatim ---- */
    for (int i = E + 1; i < count; i++)
        ir_arena_emit(&new_arena, *ir_get(c, i));

    {
        ir_arena_free(&c->ir_arena);
        c->ir_arena = new_arena;
        ok = true;
    }

fail:
    if (!ok) ir_arena_free(&new_arena);
    free(internal_pos);
    free(copy_start);
    free(copy_label);
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
                        if (lu_analyze(c, &state, &state.loops[l], &plan)) {
            bool applied = plan.partial
                ? lu_apply_partial(c, &plan)
                : lu_apply(c, &plan);
            if (applied) {
                if (plan.partial) opt_profile_count_loop_partial_unroll();
                else opt_profile_count_loop_unroll();
                loop_analysis_free(&state);
                return;
            }
        }
    }
    loop_analysis_free(&state);
}
