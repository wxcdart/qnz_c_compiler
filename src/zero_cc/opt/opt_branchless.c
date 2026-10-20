#include "opt_branchless.h"
#include "opt_profile.h"
#include "cfg.h"
#include "../ir/ir.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Branchless conditional select (Sprint 39) - see opt_branchless.h for the
 * overall pattern and the generated code.
 *
 * Each arm of the ternary must be pure value plumbing - one of the shapes
 * (V is a fresh temp, A/B pre-existing temps, S an in-place SEXTn or ZEXTn):
 *
 *   (1)  MOV_REG res, A
 *   (2a) LOAD  V, [fp+off]      ; MOV_REG res, V
 *   (2b) MOV_REG V, A           ; MOV_REG res, V
 *   (3a) LOAD  V, [fp+off]      ; S V            ; MOV_REG res, V
 *   (3b) MOV_REG V, A           ; S V            ; MOV_REG res, V
 *
 * (Shape 2a/3a are the raw ternary-lowering form for locals; 2b/3b appear
 * after copy-prop replaces the arm's frame load with a copy of a live temp.)
 *
 * Safety model:
 *  - Strict shape match: no calls/stores/jumps/labels inside the arms.
 *  - CFG predecessor checks: L_else's block has exactly the JE-block as
 *    predecessor; L_end's block has exactly {then-block, else-block}.
 *    No other control flow enters or leaves the replaced span.
 *  - Selected values must be provably GPR-resident (definition walk;
 *    float/double locals load via OP_FLOAD into XMM class and are rejected;
 *    FP opcodes are not in the provably-GPR set).
 *  - The rewrite uses only existing integer ops; the condition temp stays
 *    live (it feeds the mask), so no value is lost.
 * ========================================================================= */

#define MAX_SELECT_SITES 512

typedef struct {
    int first;      /* first arm instruction index */
    int last;       /* final MOV_REG res, val index */
    int val;        /* temp holding the arm's value (final mov's src) */
    int root;       /* GPR root: copy src temp, or -1 for a frame load */
    int load_idx;   /* frame-load index, or -1 */
    int res;        /* the arm's destination temp */
} ArmInfo;

typedef struct {
    int je_idx;      /* OP_JE t, L_else */
    int cond_idx;    /* je_idx - 1: the condition definition */
    uint8_t mask_kind; /* 0: t is 0/1 (COND_SET); 1: t is an arbitrary int */
    ArmInfo then;
    ArmInfo else_arm;
    int jmp_idx;     /* OP_JMP L_end  (then.last + 1) */
    int else_label;  /* OP_LABEL L_else (jmp_idx + 1) */
    int end_label;   /* OP_LABEL L_end  (else_arm.last + 1) */
    uint16_t t;      /* condition temp (0/1) */
} SelectSite;

/* Block containing instruction idx (blocks tile the IR exactly). */
static int cfg_block_at(const ZeroCCCFG *cfg, size_t idx) {
    for (int b = 0; b < cfg->block_count; b++) {
        if (idx >= cfg->blocks[b].start && idx < cfg->blocks[b].end) return b;
    }
    return -1;
}

/* Block that starts exactly at idx (a label site), or -1. */
static int cfg_block_starting_at(const ZeroCCCFG *cfg, size_t idx) {
    for (int b = 0; b < cfg->block_count; b++) {
        if (cfg->blocks[b].start == idx) return b;
    }
    return -1;
}

static int preds_is_single(const ZeroCCBlock *blk, int only) {
    return blk->pred_count == 1 && blk->preds[0] == only;
}

static int preds_is_pair(const ZeroCCBlock *blk, int x, int y) {
    if (blk->pred_count != 2) return 0;
    return (blk->preds[0] == x && blk->preds[1] == y) ||
           (blk->preds[0] == y && blk->preds[1] == x);
}

/* Is an OP_MOV (frame load, temp_src1 == TEMP_NONE) from a slot that holds
 * an integer/pointer/struct value? Float/double locals (XMM class) and
 * unknown slots (spills, stack args) are conservatively rejected. */
static int frame_load_is_gpr(ZeroCCCompiler *c, int32_t off) {
    for (size_t s = 0; s < c->local_count; s++) {
        if ((int32_t)c->locals[s].stack_offset == -(int32_t)off) {
            ZeroCCType ty = c->locals[s].type;
            return ty.tag != TYPE_FLOAT && ty.tag != TYPE_DOUBLE;
        }
    }
    return 0;
}

/* True for opcodes that provably produce a GPR-class value. */
static int op_defines_gpr(uint8_t op) {
    switch (op) {
        case OP_MOV_IMM:
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
        case OP_NEG: case OP_SHL: case OP_SHR: case OP_SAR:
        case OP_ADD_IMM: case OP_SUB_IMM: case OP_INC: case OP_DEC:
        case OP_LOAD_16: case OP_LOAD_32: case OP_LOAD_BYTE:
        case OP_DEREF:
        case OP_COND_SET:
        case OP_CVTTSD2SI:
        case OP_LEA:
        case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
        case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
            return 1;
        default:
            return 0;
    }
}

/* Is temp t provably GPR-class given its most recent definition before
 * index `before`? Recurses through OP_MOV_REG chains (depth-bounded).
 * Returns 1 (GPR), 0 (not provably GPR / no definition). */
static int temp_is_gpr(ZeroCCCompiler *c, uint16_t t, int before, int depth) {
    if (t == TEMP_NONE || t < 16 || depth > 128) return 0;
    for (int i = before - 1; i >= 0; i--) {
        Instr *in = ir_get(i);
        if (!in || in->temp_dest != t) continue;
        /* Most recent definition of t before `before`. */
        if (in->op == OP_MOV_REG) {
            if (in->temp_src1 == TEMP_NONE || in->temp_src1 < 16) return 0;
            if (in->temp_src2 != TEMP_NONE) return 0;
            return temp_is_gpr(c, in->temp_src1, i, depth + 1);
        }
        if (in->op == OP_MOV && in->temp_src1 == TEMP_NONE) {
            return frame_load_is_gpr(c, in->imm);
        }
        return op_defines_gpr(in->op);
    }
    return 0; /* no definition found */
}

static int is_inplace_ext(const Instr *x) {
    return (x->op == OP_SEXT8 || x->op == OP_SEXT16 || x->op == OP_SEXT32 ||
            x->op == OP_ZEXT8 || x->op == OP_ZEXT16 || x->op == OP_ZEXT32) &&
           x->temp_dest == x->temp_src1 && x->temp_dest >= 16;
}

/* Match one arm starting at `start`. The arm's final MOV_REG destination
 * must equal res_expect (0 = take whatever it is, used for the then arm).
 * Returns 1 and fills *out on success. */
static int match_arm(ZeroCCCompiler *c, int start, int res_expect, ArmInfo *out) {
    (void)c;
    Instr *x1 = ir_get(start);
    int load1 = (x1->op == OP_MOV && x1->temp_src1 == TEMP_NONE && x1->temp_dest >= 16);
    int copy1 = (x1->op == OP_MOV_REG && x1->temp_dest >= 16 && x1->temp_src1 >= 16);
    if (!load1 && !copy1) return 0;

    /* Shape 1: single-instruction arm (MOV_REG res, A) - the next
     * instruction must already terminate the arm (JMP or a label). */
    if (copy1) {
        Instr *x2 = ir_get(start + 1);
        if (x2->op == OP_JMP || x2->op == OP_LABEL) {
            if (res_expect && x1->temp_dest != res_expect) return 0;
            out->first = start;
            out->last = start;
            out->val = x1->temp_src1;
            out->root = x1->temp_src1;
            out->load_idx = -1;
            out->res = x1->temp_dest;
            return 1;
        }
    }

    /* x1 starts a value chain: V = load dest or copy dest. */
    int val = x1->temp_dest;
    int root = load1 ? -1 : x1->temp_src1;
    int load_idx = load1 ? start : -1;
    Instr *x2 = ir_get(start + 1);

    /* Shape 2: [start] [MOV_REG res, V] */
    if (x2->op == OP_MOV_REG && x2->temp_src1 == val && x2->temp_dest >= 16) {
        if (res_expect && x2->temp_dest != res_expect) return 0;
        if (x2->temp_dest == val) return 0;
        out->first = start;
        out->last = start + 1;
        out->val = val;
        out->root = root;
        out->load_idx = load_idx;
        out->res = x2->temp_dest;
        return 1;
    }

    /* Shape 3: [start] [SEXT/ZEXT V] [MOV_REG res, V] */
    if (is_inplace_ext(x2)) {
        Instr *x3 = ir_get(start + 2);
        if (x3->op == OP_MOV_REG && x3->temp_src1 == val && x3->temp_dest >= 16) {
            if (res_expect && x3->temp_dest != res_expect) return 0;
            if (x3->temp_dest == val) return 0;
            out->first = start;
            out->last = start + 2;
            out->val = val;
            out->root = root;
            out->load_idx = load_idx;
            out->res = x3->temp_dest;
            return 1;
        }
    }
    return 0;
}

/* Match one ternary-select site at JE index i.
 *
 * Two condition forms are accepted:
 *  (a) the JE is preceded by OP_COND_SET defining t (t is exactly 0/1);
 *  (b) the JE reads a plain GPR value t (arbitrary int; the register-form
 *      jump tests t == 0).  prev must be flags-neutral; t must be
 *      provably GPR-resident at the JE.
 */
static int try_match_site(ZeroCCCompiler *c, int i, SelectSite *site) {
    Instr *je = ir_get(i);
    uint16_t t = je->temp_src1;
    if (t == TEMP_NONE || t < 16 || i < 1) return 0;

    Instr *prev = ir_get(i - 1);
    uint8_t mask_kind;
    if (prev->op == OP_COND_SET && prev->temp_dest == t) {
        mask_kind = 0; /* t is 0/1 */
    } else if (prev->flags_out == 0xFF && temp_is_gpr(c, t, i, 0)) {
        mask_kind = 1; /* t is an arbitrary int, jump tests t == 0 */
    } else {
        return 0;
    }

    /* Then arm, then the unconditional jump to the merge label. */
    ArmInfo then_arm;
    if (!match_arm(c, i + 1, 0, &then_arm)) return 0;
    int jmp_idx = then_arm.last + 1;
    Instr *jmp = ir_get(jmp_idx);
    if (jmp->op != OP_JMP) return 0;
    uint16_t l_end = jmp->label_idx;

    int else_label = jmp_idx + 1;
    Instr *el = ir_get(else_label);
    if (el->op != OP_LABEL || el->label_idx != je->label_idx) return 0;

    /* Else arm (same destination), then the merge label. */
    ArmInfo else_arm;
    if (!match_arm(c, else_label + 1, then_arm.res, &else_arm)) return 0;
    int end_label = else_arm.last + 1;
    Instr *en = ir_get(end_label);
    if (en->op != OP_LABEL || en->label_idx != l_end) return 0;

    uint16_t res = then_arm.res;
    if (res == t || then_arm.val == t || else_arm.val == t ||
        then_arm.val == else_arm.val) return 0;

    /* `res` must not be read anywhere in the replaced span. */
    for (int k = i - 1; k <= else_arm.last; k++) {
        Instr *x = ir_get(k);
        if (x->temp_src1 == res || x->temp_src2 == res) return 0;
    }

    /* Values must be provably GPR-resident. */
    if (then_arm.load_idx >= 0) {
        if (!frame_load_is_gpr(c, ir_get(then_arm.load_idx)->imm)) return 0;
    } else if (!temp_is_gpr(c, then_arm.root, then_arm.first, 0)) return 0;
    if (else_arm.load_idx >= 0) {
        if (!frame_load_is_gpr(c, ir_get(else_arm.load_idx)->imm)) return 0;
    } else if (!temp_is_gpr(c, else_arm.root, else_arm.first, 0)) return 0;

    /* CFG predecessor checks. */
    if (!cfg_build()) return 0;
    ZeroCCCFG *cfg = &c->cfg_cache;
    int b_je   = cfg_block_at(cfg, (size_t)i);
    int b_then = cfg_block_at(cfg, (size_t)jmp_idx);
    int b_else = cfg_block_starting_at(cfg, (size_t)else_label);
    int b_end  = cfg_block_starting_at(cfg, (size_t)end_label);
    if (b_je < 0 || b_then < 0 || b_else < 0 || b_end < 0) return 0;
    if (!preds_is_single(&cfg->blocks[b_else], b_je)) return 0;
    if (!preds_is_pair(&cfg->blocks[b_end], b_then, b_else)) return 0;

    site->je_idx = i;
    site->cond_idx = i - 1;
    site->mask_kind = mask_kind;
    site->then = then_arm;
    site->else_arm = else_arm;
    site->jmp_idx = jmp_idx;
    site->else_label = else_label;
    site->end_label = end_label;
    site->t = t;
    return 1;
}

void opt_branchless_select(ZeroCCCompiler *c) {
    if (!c) return;
    int count = (int)ir_get_count();
    if (count < 6) return;

    SelectSite sites[MAX_SELECT_SITES];
    int n_sites = 0;
    int last_end = -1;

    for (int i = 1; i < count; i++) {
        if (ir_get(i)->op != OP_JE) continue;
        SelectSite site;
        if (!try_match_site(c, i, &site)) continue;
        if (site.je_idx <= last_end) { i = site.end_label; continue; }  /* overlap */
        sites[n_sites++] = site;
        last_end = site.end_label;
        if (n_sites >= MAX_SELECT_SITES) break;
        i = site.end_label;
    }
    if (n_sites == 0) return;

    /* 16-bit temp guard (C2b invariant): 4 fresh temps per site. */
    if (c->next_temp_id + 4 * n_sites >= TEMP_NONE) return;

    /* Two-pass rebuild (same recipe as LICM): copy everything, splicing in
     * the branchless sequence for each site; remap ir_call_args. */
    IRArena new_arena;
    if (!ir_arena_init(&new_arena, (size_t)count + (size_t)n_sites * 6 + 16)) return;

    int *old_to_new = malloc((size_t)count * sizeof(int));
    if (!old_to_new) { ir_arena_free(&new_arena); return; }
    for (int i = 0; i < count; i++) old_to_new[i] = -1;

    int s = 0;    for (int i = 0; i < count; i++) {
        /* Skip the consumed span of the site just spliced in (the main
         * loop would otherwise re-copy the removed instructions). */
        if (s > 0 && i > sites[s - 1].je_idx && i <= sites[s - 1].else_arm.last) continue;
        if (s < n_sites && i == sites[s].je_idx) {
            SelectSite *sp = &sites[s];

            /* Hoist each arm's plumbing (all arm instructions except the
             * final MOV_REG) before the select. */
            for (int k = sp->then.first; k < sp->then.last; k++)
                ir_arena_emit(&new_arena, *ir_get(k));
            for (int k = sp->else_arm.first; k < sp->else_arm.last; k++)
                ir_arena_emit(&new_arena, *ir_get(k));

            uint16_t t2   = (uint16_t)c->next_temp_id++;
            uint16_t m1   = (uint16_t)c->next_temp_id++;
            uint16_t diff = (uint16_t)c->next_temp_id++;
            uint16_t sel  = (uint16_t)c->next_temp_id++;
            /* mask = (t ? -1 : 0), built from -S-safe opcodes only
             * (asm_emitter has no OP_NOT / OP_SUB_INC / OP_ADD_INC /
             * OP_INC / OP_DEC). */
            if (sp->mask_kind == 0) {
                /* t is 0/1: mask = t * -1 */
                ir_arena_emit(&new_arena, (Instr){.op = OP_MOV_IMM, .temp_dest = m1, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = (int16_t)-1, .imm64 = -1, .label_idx = 0});
                ir_arena_emit(&new_arena, (Instr){.op = OP_MUL, .temp_dest = t2, .temp_src1 = sp->t, .temp_src2 = m1, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
            } else {
                /* t arbitrary: is_zero = (t == 0); mask = is_zero - 1 */
                ir_arena_emit(&new_arena, (Instr){.op = OP_CMP_ZERO, .temp_dest = TEMP_NONE, .temp_src1 = sp->t, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = REG_FLAGS, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
                ir_arena_emit(&new_arena, (Instr){.op = OP_COND_SET, .temp_dest = t2, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE, .cond = CC_EQ, .flags_out = 0xFF, .flags_in = REG_FLAGS, .imm = 0, .imm64 = 0, .label_idx = 0});
                ir_arena_emit(&new_arena, (Instr){.op = OP_MOV_IMM, .temp_dest = m1, .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 1, .imm64 = 1, .label_idx = 0});
                ir_arena_emit(&new_arena, (Instr){.op = OP_SUB, .temp_dest = t2, .temp_src1 = t2, .temp_src2 = m1, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
            }
            /* sel = mask & (A ^ B);  res = B ^ sel  (t=1 -> A, t=0 -> B) */
            ir_arena_emit(&new_arena, (Instr){.op = OP_XOR, .temp_dest = diff, .temp_src1 = sp->then.val, .temp_src2 = sp->else_arm.val, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
            ir_arena_emit(&new_arena, (Instr){.op = OP_AND, .temp_dest = sel, .temp_src1 = t2, .temp_src2 = diff, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});
            ir_arena_emit(&new_arena, (Instr){.op = OP_XOR, .temp_dest = sp->then.res, .temp_src1 = sp->else_arm.val, .temp_src2 = sel, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = 0});

            /* Mark the consumed old instructions (JE through the else arm's
             * final MOV_REG, inclusive of the L_else label). The L_end label
             * at end_label is kept (copied when the scan reaches it). */
            for (int k = sp->je_idx; k <= sp->else_arm.last; k++) old_to_new[k] = -1;
            s++;
            continue;
        }
        old_to_new[i] = (int)new_arena.count;
        ir_arena_emit(&new_arena, *ir_get(i));
    }

    /* Remap IRCallArgs indices (LICM recipe), then swap in the arena. */
    if (c->ir_call_args.table) {
        for (size_t ci = 0; ci < c->ir_call_args.count; ci++) {
            size_t old_pos = c->ir_call_args.table[ci].instr_index;
            if (old_pos < (size_t)count && old_to_new[old_pos] >= 0) {
                c->ir_call_args.table[ci].instr_index = (size_t)old_to_new[old_pos];
            }
        }
    }
    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;

    free(old_to_new);
    opt_profile_count_branchless();
}
