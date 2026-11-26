#include "zero_cc/opt/opt_loop_idiom.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/opt_loop.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ast_lowering_internal.h"

/* =========================================================================
 * Sprint 232: Loop Idiom Recognition & Memcpy/Memset Synthesis (Tier 4, Item #20)
 *
 * Detects countable scalar loops that initialize or copy memory:
 *   1. Memset idiom:
 *      for (int i = 0; i < n; i++) dst[i] = 0;  (or constant val)
 *      -> Lowered to memset(dst, val, n * elem_size)
 *
 *   2. Memcpy idiom:
 *      for (int i = 0; i < n; i++) dst[i] = src[i];
 *      -> Lowered to memcpy(dst, src, n * elem_size)
 *
 * Safety model:
 *  - Guard against transforming memset/memcpy themselves (infinite recursion).
 *  - Loop header test (counter < bound -> JE L_exit) is retained so 0-trip
 *    and negative-trip loops safely bypass the synthesized call.
 *  - Destination and source addresses must be affine with matching element size.
 *  - Counter variable is committed to bound value upon loop exit.
 * ========================================================================= */

typedef enum {
    IDIOM_NONE = 0,
    IDIOM_MEMSET,
    IDIOM_MEMCPY
} IdiomKind;

typedef struct {
    int H;                  /* Header index */
    int E;                  /* Backedge index */
    int bh_end;             /* Header test terminal (JE/JNE) index */
    int exit_label;         /* Exit label */
    IdiomKind kind;         /* IDIOM_MEMSET or IDIOM_MEMCPY */
    int elem_size;          /* 1, 2, 4, or 8 bytes */
    int dest_base_temp;     /* Destination base pointer temp */
    int dest_base_def;      /* In-loop definition index of dest_base (-1 if pre-loop) */
    int src_base_temp;      /* Source base pointer temp (for memcpy) */
    int src_base_def;       /* In-loop definition index of src_base (-1 if pre-loop) */
    int val_temp;           /* Value temp (for memset) */
    int64_t val_const;      /* Constant value (if constant) */
    bool val_is_const;
    int bound_temp;         /* Bound temp (TEMP_NONE if constant) */
    int64_t bound_const;    /* Constant bound value */
    bool bound_is_const;
    int64_t counter_slot;   /* Counter stack slot (0x7FFFFFFF if promoted vreg) */
    int counter_temp;       /* Counter virtual register */
} IdiomPlan;

/* Helper: trace temp definition backward within a range */
static int idiom_find_def(ZeroCCCompiler *c, int start, int end, uint32_t temp) {
    if (temp == TEMP_NONE) return -1;
    for (int i = end; i >= start; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_dest == temp) return i;
    }
    return -1;
}

/* Helper: check if a temp is invariant across a loop span */
static bool idiom_temp_is_invariant(ZeroCCCompiler *c, int H, int E, uint32_t temp) {
    if (temp == TEMP_NONE) return false;
    for (int i = H; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_dest == temp) return false;
    }
    return true;
}

/* Helper: extract base pointer and scale from an address temporary */
static bool idiom_analyze_addr(ZeroCCCompiler *c, int H, int pos, int addr_temp,
                               int counter_temp, int64_t counter_slot,
                               int *out_base_temp, int *out_base_def, int *out_elem_size) {
    if (addr_temp == TEMP_NONE || pos <= H) return false;

    int def = idiom_find_def(c, H, pos - 1, (uint32_t)addr_temp);
    if (def < 0) return false;
    Instr *ins = ir_get(c, def);
    if (!ins) return false;

    if (ins->op == OP_ADD) {
        uint32_t op1 = ins->temp_src1;
        uint32_t op2 = ins->temp_src2;

        /* One operand should be the base pointer, the other the offset */
        for (int pass = 0; pass < 2; pass++) {
            uint32_t cand_base = (pass == 0) ? op1 : op2;
            uint32_t cand_off = (pass == 0) ? op2 : op1;

            /* Check if cand_base is invariant */
            bool base_ok = false;
            int base_def = idiom_find_def(c, H, def - 1, cand_base);
            if (base_def < 0) {
                base_ok = idiom_temp_is_invariant(c, H, pos, cand_base);
            } else {
                Instr *bi = ir_get(c, base_def);
                if (bi && (bi->op == OP_MOV || bi->op == OP_LEA || bi->op == OP_LEA_RIP || bi->op == OP_MOV_GLOBAL)) {
                    base_ok = true;
                }
            }
            if (!base_ok) continue;

            /* Check if cand_off matches counter (with optional SHL) */
            int elem_size = 1;
            uint32_t off_var = cand_off;
            int curr_pos = def;
            bool matches_counter = false;

            while (curr_pos > H) {
                if (counter_temp >= 0 && (int)off_var == counter_temp) {
                    matches_counter = true;
                    break;
                }
                int off_def = idiom_find_def(c, H, curr_pos - 1, off_var);
                if (off_def < 0) break;
                Instr *oi = ir_get(c, off_def);
                if (!oi) break;
                if (oi->op == OP_MOV && counter_slot != 0x7FFFFFFF &&
                    (oi->imm64 == counter_slot || oi->imm == (int16_t)counter_slot)) {
                    matches_counter = true;
                    break;
                }
                if (oi->op == OP_SHL) {
                    int sh_def = (oi->temp_src2 != TEMP_NONE) ? idiom_find_def(c, H, off_def - 1, oi->temp_src2) : -1;
                    Instr *sh_val = (sh_def >= 0) ? ir_get(c, sh_def) : NULL;
                    int64_t sh = -1;
                    if (sh_val && sh_val->op == OP_MOV_IMM) sh = sh_val->imm64;
                    if (sh == 1) elem_size = 2;
                    else if (sh == 2) elem_size = 4;
                    else if (sh == 3) elem_size = 8;
                    else break;
                    off_var = oi->temp_src1;
                    curr_pos = off_def;
                } else if (oi->op == OP_SEXT32 || oi->op == OP_ZEXT32 || oi->op == OP_MOV_REG) {
                    off_var = oi->temp_src1;
                    curr_pos = off_def;
                } else {
                    break;
                }
            }

            if (matches_counter) {
                *out_base_temp = (int)cand_base;
                *out_base_def = base_def;
                *out_elem_size = elem_size;
                return true;
            }
        }
    }

    return false;
}

/* Helper: determine if two base pointers are distinct objects */
static bool idiom_bases_are_distinct(ZeroCCCompiler *c, int src_base, int src_base_def,
                                     int dest_base, int dest_base_def) {
    if (src_base_def >= 0 && dest_base_def >= 0 && src_base_def != dest_base_def) {
        Instr *bsrc = ir_get(c, src_base_def);
        Instr *bdst = ir_get(c, dest_base_def);
        if (bsrc && bdst) {
            if (bsrc->op == OP_MOV && bdst->op == OP_MOV) {
                return (bsrc->imm64 != bdst->imm64);
            }
            if (bsrc->op == OP_LEA && bdst->op == OP_LEA) {
                return (bsrc->imm64 != bdst->imm64);
            }
            return (bsrc->op != bdst->op);
        }
    }
    return (src_base != dest_base);
}

/* Analyze a loop candidate for memset or memcpy idioms */
static bool idiom_analyze_loop(ZeroCCCompiler *c, LoopInfo *loop, IdiomPlan *plan) {
    int H = loop->header_idx;
    int E = loop->exit_idx;
    int count = (int)ir_get_count(c);
    if (H < 0 || E < 0 || E >= count || E <= H) return false;

    memset(plan, 0, sizeof(*plan));
    plan->H = H;
    plan->E = E;
    plan->counter_slot = 0x7FFFFFFF;
    plan->counter_temp = -1;
    plan->bound_temp = TEMP_NONE;
    plan->dest_base_def = -1;
    plan->src_base_def = -1;

    /* Do not optimize memset/memcpy themselves */
    ZeroCCFunction *cur_fn = zero_cc_get_current_fn(c);
    if (cur_fn && (strcmp(cur_fn->name, "memset") == 0 ||
                   strcmp(cur_fn->name, "memcpy") == 0 ||
                   strcmp(cur_fn->name, "memmove") == 0)) {
        return false;
    }

    /* Step 1: Find header branch terminal (JE/JNE) */
    int bh_end = -1;
    for (int i = H + 1; i <= E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_JE || ins->op == OP_JNE) {
            bh_end = i;
            plan->exit_label = (int)ins->label_idx;
            break;
        }
        if (ins->op == OP_CALL || ins->op == OP_SYSCALL || ins->op == OP_RET) return false;
    }
    if (bh_end < 0) return false;
    plan->bh_end = bh_end;

    /* Step 2: Find comparison (CMP) and condition (COND_SET) */
    int cmp_idx = -1;
    int cs_idx = -1;
    for (int i = bh_end - 1; i > H; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_COND_SET && cs_idx < 0) cs_idx = i;
        if (ins->op == OP_CMP && cmp_idx < 0) { cmp_idx = i; break; }
    }
    if (cmp_idx < 0 || cs_idx < 0) return false;

    Instr *cm = ir_get(c, cmp_idx);
    Instr *cs = ir_get(c, cs_idx);
    if (!cm || !cs) return false;

    /* Identify counter and bound from CMP operands */
    uint32_t op1 = cm->temp_src1;
    uint32_t op2 = cm->temp_src2;

    /* Find counter update in backedge block */
    int64_t counter_slot = 0x7FFFFFFF;
    int counter_temp = -1;
    for (int i = E - 1; i > bh_end; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_MOV_MEM) {
            counter_slot = ins->imm64 ? ins->imm64 : ins->imm;
            break;
        }
        if (ins->op == OP_ADD && (int)ins->temp_dest == (int)op1) {
            counter_temp = (int)ins->temp_dest;
            break;
        }
    }
    plan->counter_slot = counter_slot;
    plan->counter_temp = counter_temp;

    /* Check whether op1 or op2 traces to counter */
    bool op1_is_counter = false;
    if (counter_temp >= 0 && (int)op1 == counter_temp) {
        op1_is_counter = true;
    } else if (counter_slot != 0x7FFFFFFF) {
        /* Trace op1 backward to see if it loaded from counter_slot */
        int cur = cmp_idx;
        uint32_t trace_v = op1;
        while (cur > H) {
            int d = idiom_find_def(c, H, cur - 1, trace_v);
            if (d < 0) break;
            Instr *di = ir_get(c, d);
            if (!di) break;
            if (di->op == OP_MOV && (di->imm64 == counter_slot || di->imm == (int16_t)counter_slot)) {
                op1_is_counter = true;
                break;
            }
            if (di->op == OP_SEXT32 || di->op == OP_ZEXT32 || di->op == OP_MOV_REG) {
                trace_v = di->temp_src1;
                cur = d;
            } else {
                break;
            }
        }
    }
    if (!op1_is_counter && counter_temp < 0 && counter_slot == 0x7FFFFFFF) {
        /* Default: in a canonical loop (i < bound), op1 is counter */
        op1_is_counter = true;
    }

    /* Bound detection */
    uint32_t bound_op = op1_is_counter ? op2 : op1;
    int bound_def = idiom_find_def(c, 0, cmp_idx - 1, bound_op);
    if (bound_def >= 0) {
        Instr *bi = ir_get(c, bound_def);
        if (bi && bi->op == OP_MOV_IMM) {
            plan->bound_is_const = true;
            plan->bound_const = bi->imm64;
        } else {
            plan->bound_temp = (int)bound_op;
        }
    } else {
        plan->bound_temp = (int)bound_op;
    }

    /* Step 3: Scan loop body for single store (memset) or load + store (memcpy) */
    int store_idx = -1;
    int load_idx = -1;
    int store_count = 0;
    int load_count = 0;

    for (int i = bh_end + 1; i < E; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;

        if (ins->op == OP_CALL || ins->op == OP_CALL_INDIRECT ||
            ins->op == OP_SYSCALL || ins->op == OP_CLONE ||
            ins->op == OP_ASM || ins->op == OP_SETJMP) {
            return false;
        }

        bool is_store = (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
                         ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
                         ins->op == OP_STORE64 || ins->op == OP_FSTORE ||
                         ins->op == OP_VSTORESS || ins->op == OP_VSTORESD);

        bool is_load = (ins->op == OP_LOAD_BYTE || ins->op == OP_LOAD_16 ||
                        ins->op == OP_LOAD_32 || ins->op == OP_LOAD64 ||
                        ins->op == OP_FLOAD || ins->op == OP_VLOADSS ||
                        ins->op == OP_VLOADSD);

        if (is_store) {
            store_idx = i;
            store_count++;
        }
        if (is_load) {
            load_idx = i;
            load_count++;
        }
    }

    if (store_count != 1) return false;

    Instr *st = ir_get(c, store_idx);
    bool is_int_store = (st->op == OP_STORE_BYTE || st->op == OP_STORE_16 ||
                         st->op == OP_STORE_32 || st->op == OP_STORE_64 ||
                         st->op == OP_STORE64);
    uint32_t dest_addr = is_int_store ? st->temp_src2 : st->temp_src1;
    uint32_t val_stored = is_int_store ? st->temp_src1 : st->temp_src2;

    int dest_base = -1;
    int dest_base_def = -1;
    int dest_elem_size = 1;
    if (st->op == OP_STORE_16) dest_elem_size = 2;
    else if (st->op == OP_STORE_32 || st->op == OP_VSTORESS) dest_elem_size = 4;
    else if (st->op == OP_STORE_64 || st->op == OP_STORE64 || st->op == OP_FSTORE || st->op == OP_VSTORESD) dest_elem_size = 8;

    int analyzed_size = 1;
    if (!idiom_analyze_addr(c, H, store_idx, (int)dest_addr, counter_temp, counter_slot,
                            &dest_base, &dest_base_def, &analyzed_size)) {
        return false;
    }
    if (analyzed_size > dest_elem_size) dest_elem_size = analyzed_size;
    plan->elem_size = dest_elem_size;
    plan->dest_base_temp = dest_base;
    plan->dest_base_def = dest_base_def;

    /* Check for Memcpy idiom */
    if (load_count == 1 && load_idx >= 0) {
        Instr *ld = ir_get(c, load_idx);
        bool val_matches = false;
        if (ld) {
            uint32_t v_curr = val_stored;
            if (v_curr == (uint32_t)ld->temp_dest) {
                val_matches = true;
            } else {
                while (v_curr != TEMP_NONE && v_curr != (uint32_t)ld->temp_dest) {
                    int v_def = idiom_find_def(c, H, store_idx - 1, v_curr);
                    if (v_def < 0 || v_def <= load_idx) break;
                    Instr *vi = ir_get(c, v_def);
                    if (!vi) break;
                    if (vi->op == OP_SEXT8 || vi->op == OP_SEXT16 || vi->op == OP_SEXT32 ||
                        vi->op == OP_ZEXT8 || vi->op == OP_ZEXT16 || vi->op == OP_ZEXT32 ||
                        vi->op == OP_MOV_REG) {
                        v_curr = vi->temp_src1;
                        if (v_curr == (uint32_t)ld->temp_dest) {
                            val_matches = true;
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }
        if (val_matches) {
            int src_base = -1;
            int src_base_def = -1;
            int src_elem_size = 1;
            if (!idiom_analyze_addr(c, H, load_idx, (int)ld->temp_src1, counter_temp, counter_slot,
                                    &src_base, &src_base_def, &src_elem_size)) {
                return false;
            }
            if (idiom_bases_are_distinct(c, src_base, src_base_def, dest_base, dest_base_def)) {
                plan->kind = IDIOM_MEMCPY;
                plan->src_base_temp = src_base;
                plan->src_base_def = src_base_def;
                return true;
            }
        }
    }

    /* Check for Memset idiom */
    if (load_count == 0) {
        int val_def = idiom_find_def(c, 0, store_idx - 1, val_stored);
        if (plan->elem_size == 1) {
            if (val_def >= 0) {
                Instr *vi = ir_get(c, val_def);
                if (vi && vi->op == OP_MOV_IMM) {
                    plan->kind = IDIOM_MEMSET;
                    plan->val_is_const = true;
                    plan->val_const = vi->imm64;
                    plan->val_temp = (int)val_stored;
                    return true;
                }
            }
            if (idiom_temp_is_invariant(c, H, E, val_stored)) {
                plan->kind = IDIOM_MEMSET;
                plan->val_is_const = false;
                plan->val_temp = (int)val_stored;
                return true;
            }
        } else {
            /* Multi-byte elements (int, double, etc.) can only use memset if filling with 0 */
            if (val_def >= 0) {
                Instr *vi = ir_get(c, val_def);
                if (vi && vi->op == OP_MOV_IMM && vi->imm64 == 0) {
                    plan->kind = IDIOM_MEMSET;
                    plan->val_is_const = true;
                    plan->val_const = 0;
                    plan->val_temp = (int)val_stored;
                    return true;
                }
            }
        }
    }

    return false;
}

/* Ensure helper functions are registered in the compiler symbol table */
static void idiom_ensure_fn_declared(ZeroCCCompiler *c, const char *name) {
    if (!zero_cc_find_function(c, name) && c->function_count < ZERO_CC_MAX_FUNCTIONS) {
        ZeroCCFunction *fn = &c->functions[c->function_count++];
        memset(fn, 0, sizeof(*fn));
        snprintf(fn->name, sizeof(fn->name), "%s", name);
        fn->is_prototype = true;
        fn->is_external = true;
    }
}

/* Transform qualifying loop using two-pass arena rebuild */
static bool idiom_apply_plan(ZeroCCCompiler *c, const IdiomPlan *plan) {
    int count = (int)ir_get_count(c);
    int E = plan->E;
    int bh_end = plan->bh_end;

    idiom_ensure_fn_declared(c, (plan->kind == IDIOM_MEMCPY) ? "memcpy" : "memset");

    IRArena new_arena;
    ir_arena_init(&new_arena, (size_t)(count + 32));

    /* Pass 1: Copy instructions up to and including the loop test (bh_end) */
    for (int i = 0; i <= bh_end; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    /* Pass 2: In the then-block, emit base pointer loads if they were defined in-loop */
    int dest_base_for_call = plan->dest_base_temp;
    if (plan->dest_base_def >= 0) {
        Instr *bdef = ir_get(c, plan->dest_base_def);
        int dest_arg = zc_alloc_temp(c);
        zc_temp_tag_set(c, dest_arg, ZC_TAG_PTR);
        Instr new_ins = *bdef;
        new_ins.temp_dest = dest_arg;
        ir_arena_emit(&new_arena, new_ins);
        dest_base_for_call = dest_arg;
    }

    int src_base_for_call = plan->src_base_temp;
    if (plan->kind == IDIOM_MEMCPY && plan->src_base_def >= 0) {
        Instr *sdef = ir_get(c, plan->src_base_def);
        int src_arg = zc_alloc_temp(c);
        zc_temp_tag_set(c, src_arg, ZC_TAG_PTR);
        Instr new_s_ins = *sdef;
        new_s_ins.temp_dest = src_arg;
        ir_arena_emit(&new_arena, new_s_ins);
        src_base_for_call = src_arg;
    }

    int bytes_temp = zc_alloc_temp(c);
    zc_temp_tag_set(c, bytes_temp, ZC_TAG_INT(ZC_TAGW_8));

    if (plan->bound_is_const) {
        int64_t total_bytes = plan->bound_const * plan->elem_size;
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_MOV_IMM, .temp_dest = bytes_temp,
            .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
            .flags_out = 0xFF, .flags_in = 0xFF,
            .imm = (int16_t)total_bytes, .imm64 = total_bytes
        });
    } else {
        if (plan->elem_size == 1) {
            bytes_temp = plan->bound_temp;
        } else {
            int shift = (plan->elem_size == 8) ? 3 : (plan->elem_size == 4) ? 2 : 1;
            int sh_temp = zc_alloc_temp(c);
            zc_temp_tag_set(c, sh_temp, ZC_TAG_INT(ZC_TAGW_4));
            ir_arena_emit(&new_arena, (Instr){
                .op = OP_MOV_IMM, .temp_dest = sh_temp,
                .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
                .flags_out = 0xFF, .flags_in = 0xFF,
                .imm = (int16_t)shift, .imm64 = shift
            });
            ir_arena_emit(&new_arena, (Instr){
                .op = OP_SHL, .temp_dest = bytes_temp,
                .temp_src1 = plan->bound_temp, .temp_src2 = sh_temp,
                .flags_out = 0xFF, .flags_in = 0xFF
            });
        }
    }

    if (plan->kind == IDIOM_MEMSET) {
        int val_arg = plan->val_temp;
        if (plan->val_is_const) {
            val_arg = zc_alloc_temp(c);
            zc_temp_tag_set(c, val_arg, ZC_TAG_INT(ZC_TAGW_4));
            ir_arena_emit(&new_arena, (Instr){
                .op = OP_MOV_IMM, .temp_dest = val_arg,
                .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
                .flags_out = 0xFF, .flags_in = 0xFF,
                .imm = (int16_t)plan->val_const, .imm64 = plan->val_const
            });
        }

        /* OP_CALL for memset(dest, val, bytes) */
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_CALL, .temp_dest = 3,
            .temp_src1 = dest_base_for_call, .temp_src2 = val_arg,
            .imm = IR_CALL_ARG01_REG, .imm64 = (int64_t)(intptr_t)"memset",
            .flags_out = 0xFF, .flags_in = 0xFF
        });
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_CALL_ARG, .temp_dest = TEMP_NONE,
            .temp_src1 = bytes_temp, .temp_src2 = TEMP_NONE,
            .cond = 0, .width = 255, .imm = 255,
            .flags_out = 0xFF, .flags_in = 0xFF
        });
    } else if (plan->kind == IDIOM_MEMCPY) {
        /* OP_CALL for memcpy(dest, src, bytes) */
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_CALL, .temp_dest = 3,
            .temp_src1 = dest_base_for_call, .temp_src2 = src_base_for_call,
            .imm = IR_CALL_ARG01_REG, .imm64 = (int64_t)(intptr_t)"memcpy",
            .flags_out = 0xFF, .flags_in = 0xFF
        });
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_CALL_ARG, .temp_dest = TEMP_NONE,
            .temp_src1 = bytes_temp, .temp_src2 = TEMP_NONE,
            .cond = 0, .width = 255, .imm = 255,
            .flags_out = 0xFF, .flags_in = 0xFF
        });
    }

    /* Update counter slot upon completion */
    if (plan->counter_slot != 0x7FFFFFFF) {
        int fin_temp = plan->bound_temp;
        if (plan->bound_is_const) {
            fin_temp = zc_alloc_temp(c);
            zc_temp_tag_set(c, fin_temp, ZC_TAG_INT(ZC_TAGW_4));
            ir_arena_emit(&new_arena, (Instr){
                .op = OP_MOV_IMM, .temp_dest = fin_temp,
                .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
                .flags_out = 0xFF, .flags_in = 0xFF,
                .imm = (int16_t)plan->bound_const, .imm64 = plan->bound_const
            });
        }
        ir_arena_emit(&new_arena, (Instr){
            .op = OP_MOV_MEM, .temp_dest = TEMP_NONE,
            .temp_src1 = fin_temp, .temp_src2 = TEMP_NONE,
            .imm = (int16_t)plan->counter_slot, .imm64 = plan->counter_slot,
            .flags_out = 0xFF, .flags_in = 0xFF
        });
    }

    /* Jump directly to exit label, bypassing the loop body and backedge */
    ir_arena_emit(&new_arena, (Instr){
        .op = OP_JMP, .temp_dest = TEMP_NONE,
        .temp_src1 = TEMP_NONE, .temp_src2 = TEMP_NONE,
        .label_idx = (uint32_t)plan->exit_label,
        .flags_out = 0xFF, .flags_in = 0xFF
    });

    /* Copy instructions after loop backedge E */
    for (int i = E + 1; i < count; i++) {
        ir_arena_emit(&new_arena, *ir_get(c, i));
    }

    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;
    return true;
}

void opt_loop_idiom(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;

    LoopAnalysisState state;
    loop_analysis_init(&state);
    loop_detect(c, &state);

    if (state.loop_count == 0) {
        loop_analysis_free(&state);
        return;
    }

    for (int l = 0; l < state.loop_count; l++) {
        IdiomPlan plan;
        if (idiom_analyze_loop(c, &state.loops[l], &plan)) {
            if (idiom_apply_plan(c, &plan)) {
                loop_analysis_free(&state);
                return;
            }
        }
    }

    loop_analysis_free(&state);
}
