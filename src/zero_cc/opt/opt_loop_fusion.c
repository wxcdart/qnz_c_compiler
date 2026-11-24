/*
 * Loop Fusion / Jamming Optimization Pass (Sprint 215)
 *
 * Merges adjacent loops with identical iteration spaces and compatible data
 * dependencies into a single loop, eliminating redundant loop index increments,
 * comparisons, and branch overhead, and enabling downstream passes (GVN store-to-load
 * forwarding, SLP, and SIMD auto-vectorization) to co-optimize the combined loop body.
 */

#include "zero_cc/opt/opt_loop_fusion.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/opt/cfg.h"
#include "zero_cc/opt/opt_profile.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emitter.h"

#define MAX_FUSIBLE_LOOPS 32

typedef struct {
    int header_label;
    int exit_label;
    int start_idx;       /* header OP_LABEL instruction index */
    int test_idx;        /* conditional exit jump instruction index */
    int body_start_idx;  /* first body instruction */
    int body_end_idx;    /* last body instruction (before step) */
    int step_idx;        /* induction variable increment instruction */
    int backedge_idx;    /* unconditional jump back to header */
    int exit_idx;        /* position of exit label in IR */
    uint32_t var_temp;   /* induction variable virtual register */
    uint32_t bound_temp; /* loop bound virtual register */
    int64_t bound_const; /* constant bound if bound_temp == TEMP_NONE */
    int64_t init_const;  /* constant initial induction value */
    bool has_const_bound;
    bool has_const_init;
} FusibleLoopInfo;

/* Check if instruction is a branch or terminal */
static inline bool is_terminal_or_branch(Opcode op) {
    return (op == OP_LABEL || op == OP_JMP || op == OP_JE || op == OP_JNE ||
            op == OP_JL || op == OP_JLE || op == OP_JG || op == OP_JCOND ||
            op == OP_RET || op == OP_EPILOGUE || op == OP_CALL ||
            op == OP_CALL_INDIRECT || op == OP_SYSCALL || op == OP_CLONE ||
            op == OP_ASM || op == OP_SETJMP || op == OP_LONGJMP);
}

/* Find label position in IR */
static int find_label_pos(ZeroCCCompiler *c, int label_idx) {
    size_t count = ir_get_count(c);
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx == label_idx) {
            return (int)i;
        }
    }
    return -1;
}

/* Analyze a loop candidate to check if it has standard for/while structure */
static bool analyze_fusible_loop(ZeroCCCompiler *c, int start, int end, FusibleLoopInfo *info) {
    if (end - start < 4) return false;

    Instr *hdr = ir_get(c, start);
    if (!hdr || hdr->op != OP_LABEL) return false;

    Instr *back = ir_get(c, end);
    if (!back || back->op != OP_JMP || (int)back->label_idx != (int)hdr->label_idx) return false;

    info->header_label = (int)hdr->label_idx;
    info->start_idx = start;
    info->backedge_idx = end;

    /* Locate exit test jump in header region */
    int test_idx = -1;
    int exit_label = -1;
    for (int i = start + 1; i <= start + 8 && i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            test_idx = i;
            exit_label = (int)ins->label_idx;
            break;
        }
    }
    if (test_idx < 0 || exit_label < 0) return false;

    int exit_pos = find_label_pos(c, exit_label);
    if (exit_pos < end) return false; /* exit must be outside loop */

    info->test_idx = test_idx;
    info->exit_label = exit_label;
    info->exit_idx = exit_pos;

    /* Identify induction increment right before backedge */
    int step_idx = -1;
    uint32_t var_temp = TEMP_NONE;
    for (int i = end - 1; i > test_idx; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if ((ins->op == OP_ADD || ins->op == OP_ADD_IMM) && ins->temp_dest != TEMP_NONE) {
            /* Check if adding constant 1 */
            if (ins->op == OP_ADD_IMM && (ins->imm == 1 || ins->imm64 == 1)) {
                step_idx = i;
                var_temp = ins->temp_dest;
                break;
            } else if (ins->op == OP_ADD) {
                step_idx = i;
                var_temp = ins->temp_dest;
                break;
            }
        }
        /* If we hit other complex instructions, stop searching for step */
        if (ins->op == OP_STORE_32 || ins->op == OP_STORE_64 || ins->op == OP_LOAD_32 || ins->op == OP_LOAD64 || ins->op == OP_DEREF) {
            break;
        }
    }
    if (step_idx < 0 || var_temp == TEMP_NONE) return false;

    info->step_idx = step_idx;
    info->var_temp = var_temp;
    info->body_start_idx = test_idx + 1;
    info->body_end_idx = step_idx - 1;
    if (info->body_end_idx < info->body_start_idx) return false;

    /* Find comparison instruction before test_idx */
    int cmp_idx = -1;
    for (int i = test_idx - 1; i >= start; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_CMP) {
            cmp_idx = i;
            break;
        }
    }
    if (cmp_idx < 0) return false;

    Instr *cmp_ins = ir_get(c, cmp_idx);
    uint32_t op1 = cmp_ins->temp_src1;
    uint32_t op2 = cmp_ins->temp_src2;

    info->has_const_bound = false;
    info->bound_const = 0;
    info->bound_temp = TEMP_NONE;

    if (op1 == var_temp) {
        info->bound_temp = op2;
    } else if (op2 == var_temp) {
        info->bound_temp = op1;
    } else {
        /* Check if one operand traces back to var_temp through SEXT/MOV */
        info->bound_temp = op2;
    }

    /* Check if bound_temp is a constant definition before loop */
    for (int i = 0; i < start; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->temp_dest == info->bound_temp && ins->op == OP_MOV_IMM) {
            info->has_const_bound = true;
            info->bound_const = ins->imm64 ? ins->imm64 : ins->imm;
            break;
        }
    }

    /* Check if induction variable is initialized to a constant before loop */
    info->has_const_init = false;
    info->init_const = 0;
    for (int i = start - 1; i >= 0 && i >= start - 16; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->temp_dest == var_temp && ins->op == OP_MOV_IMM) {
            info->has_const_init = true;
            info->init_const = ins->imm64 ? ins->imm64 : ins->imm;
            break;
        }
    }

    /* Verify body instructions purity */
    for (int i = info->body_start_idx; i <= info->body_end_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if (is_terminal_or_branch(ins->op)) return false;
    }

    return true;
}

/* Check if two loops have compatible data dependencies */
static bool check_fusion_safety(ZeroCCCompiler *c, FusibleLoopInfo *l1, FusibleLoopInfo *l2) {
    /* Check bounds match */
    if (l1->has_const_bound && l2->has_const_bound) {
        if (l1->bound_const != l2->bound_const) return false;
    } else if (l1->bound_temp != l2->bound_temp) {
        return false;
    }

    /* Check induction initial value matches */
    if (l1->has_const_init && l2->has_const_init) {
        if (l1->init_const != l2->init_const) return false;
    }

    /* Between L1 exit and L2 start, only allow pure setup/induction initialization */
    for (int i = l1->exit_idx + 1; i < l2->start_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP) continue;
        if (is_terminal_or_branch(ins->op)) return false;
        if (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
            ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
            ins->op == OP_FSTORE || ins->op == OP_VSTORE ||
            ins->op == OP_VSTORESS || ins->op == OP_VSTORESD ||
            ins->op == OP_MOV_MEM) {
            return false;
        }
    }

    /* Collect store target addresses in Loop 1 */
    uint32_t l1_store_addrs[32];
    int l1_store_count = 0;
    for (int i = l1->body_start_idx; i <= l1->body_end_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
            ins->op == OP_FSTORE || ins->op == OP_VSTORESS ||
            ins->op == OP_VSTORESD) {
            if (l1_store_count < 32) {
                l1_store_addrs[l1_store_count++] = ins->temp_src2;
            }
        }
    }

    /* Check Loop 2 stores: Loop 2 must not store to memory read or written by Loop 1 */
    for (int i = l2->body_start_idx; i <= l2->body_end_idx; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
            ins->op == OP_FSTORE || ins->op == OP_VSTORESS ||
            ins->op == OP_VSTORESD) {
            uint32_t l2_store_addr = ins->temp_src2;

            /* Check WAW conflicts with Loop 1 stores */
            for (int k = 0; k < l1_store_count; k++) {
                if (l1_store_addrs[k] == l2_store_addr) {
                    return false; /* WAW conflict */
                }
            }

            /* Verify against all loads in Loop 1 (anti-dependency WAR) */
            for (int j = l1->body_start_idx; j <= l1->body_end_idx; j++) {
                Instr *l1_ins = ir_get(c, j);
                if (!l1_ins) continue;
                if (l1_ins->op == OP_LOAD_32 || l1_ins->op == OP_LOAD64 ||
                    l1_ins->op == OP_DEREF || l1_ins->op == OP_FLOAD ||
                    l1_ins->op == OP_VLOADSS || l1_ins->op == OP_VLOADSD) {
                    if (l1_ins->temp_src1 == l2_store_addr) {
                        return false; /* Anti-dependency detected */
                    }
                }
            }
        }
    }

    return true;
}

/* Execute loop fusion transformation */
static bool fuse_loops(ZeroCCCompiler *c, FusibleLoopInfo *l1, FusibleLoopInfo *l2) {
    size_t count = ir_get_count(c);
    int l2_body_len = l2->body_end_idx - l2->body_start_idx + 1;

    IRArena new_arena;
    if (!ir_arena_init(&new_arena, count + l2_body_len + 16)) {
        return false;
    }

    /* Build combined IR stream */
    for (size_t i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;

        if ((int)i == l1->start_idx) {
            /* Hoist pure setup instructions between L1 and L2 before L1 starts */
            for (int k = l1->exit_idx + 1; k < l2->start_idx; k++) {
                Instr *setup = ir_get(c, k);
                if (!setup || setup->op == OP_NOP) continue;
                if (setup->temp_dest != l2->var_temp) {
                    ir_arena_emit(&new_arena, *setup);
                }
            }
        }

        if ((int)i == l1->test_idx) {
            /* Redirect Loop 1 exit jump directly to Loop 2's exit label */
            Instr redirected_test = *ins;
            redirected_test.label_idx = (uint16_t)l2->exit_label;
            ir_arena_emit(&new_arena, redirected_test);
            continue;
        }

        if ((int)i == l1->step_idx) {
            /* Splice Loop 2's body right before Loop 1's induction step */
            for (int k = l2->body_start_idx; k <= l2->body_end_idx; k++) {
                Instr *l2_ins = ir_get(c, k);
                if (!l2_ins || l2_ins->op == OP_NOP) continue;
                Instr spliced = *l2_ins;
                /* Remap Loop 2's induction variable uses to Loop 1's induction variable */
                if (spliced.temp_src1 == l2->var_temp) spliced.temp_src1 = l1->var_temp;
                if (spliced.temp_src2 == l2->var_temp) spliced.temp_src2 = l1->var_temp;
                ir_arena_emit(&new_arena, spliced);
            }
            /* Emit Loop 1's step */
            ir_arena_emit(&new_arena, *ins);
            continue;
        }

        /* NOP out hoisted setup instructions between L1 and L2 */
        if ((int)i > l1->exit_idx && (int)i < l2->start_idx) {
            Instr nop = {
                .op = OP_NOP,
                .temp_dest = TEMP_NONE,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = 0,
                .imm64 = 0,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, nop);
            continue;
        }

        /* NOP out Loop 2's instructions (header through backedge) */
        if ((int)i >= l2->start_idx && (int)i <= l2->backedge_idx) {
            Instr nop = {
                .op = OP_NOP,
                .temp_dest = TEMP_NONE,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = 0,
                .imm64 = 0,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, nop);
            continue;
        }

        /* NOP out Loop 1's old exit label */
        if ((int)i == l1->exit_idx) {
            Instr nop = {
                .op = OP_NOP,
                .temp_dest = TEMP_NONE,
                .temp_src1 = TEMP_NONE,
                .temp_src2 = TEMP_NONE,
                .cond = 0,
                .flags_out = 0xFF,
                .flags_in = 0xFF,
                .imm = 0,
                .imm64 = 0,
                .label_idx = 0
            };
            ir_arena_emit(&new_arena, nop);
            continue;
        }

        /* Emit original instruction */
        ir_arena_emit(&new_arena, *ins);
    }

    ir_arena_free(&c->ir_arena);
    c->ir_arena = new_arena;
    c->cfg_version++;
    return true;
}

/* Public entry point: Loop Fusion Optimization Pass */
void opt_loop_fusion(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return;
    if (!cfg_build(c)) return;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->loop_count < 2) return;

    FusibleLoopInfo loops[MAX_FUSIBLE_LOOPS];
    int fusible_count = 0;

    int nloops = cfg->loop_count > MAX_FUSIBLE_LOOPS ? MAX_FUSIBLE_LOOPS : cfg->loop_count;
    for (int l = 0; l < nloops; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || h >= cfg->block_count || u < 0 || u >= cfg->block_count) continue;

        int start = (int)cfg->blocks[h].start;
        int end = (int)cfg->blocks[u].end - 1;

        if (analyze_fusible_loop(c, start, end, &loops[fusible_count])) {
            fusible_count++;
        }
    }

    if (fusible_count < 2) return;

    /* Scan adjacent pairs for fusion */
    for (int i = 0; i < fusible_count - 1; i++) {
        FusibleLoopInfo *l1 = &loops[i];
        FusibleLoopInfo *l2 = &loops[i + 1];

        if (l1->exit_idx <= l2->start_idx && check_fusion_safety(c, l1, l2)) {
            if (fuse_loops(c, l1, l2)) {
                /* Loop fused successfully! Re-run pass pipeline will pick up combined body */
                break;
            }
        }
    }
}
