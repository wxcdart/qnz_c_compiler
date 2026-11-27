/*
 * Aggressive Dead Code Elimination (ADCE) - Sprint 230
 *
 * Implements classical reverse-mark worklist algorithm over dataflow
 * and control-dependence graph (CDG) constructed from post-dominators.
 */

#include "zero_cc/opt/opt_adce.h"
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/types/symbols.h"
#include "zero_cc/opt/cfg.h"

#define MAX_ADCE_VREGS 65536

typedef struct {
    int *blocks;
    int count;
    int cap;
} CDList;

typedef struct {
    int *defs;
    int count;
    int cap;
} TempDefs;

static bool instr_is_adce_root(Instr *instr) {
    if (!instr) return false;
    switch (instr->op) {
        /* Memory writes have observable side effects */
        case OP_STORE_BYTE:
        case OP_STORE_16:
        case OP_STORE_32:
        case OP_STORE_64:
        case OP_FSTORE:
        case OP_VSTORE:
        case OP_V256_STORE:
        case OP_V512_STORE:
        case OP_V512_MASK_STORE:
        case OP_KMOVW_TO_K1:
        case OP_SVE_ST1:
        case OP_RVV_VSE:
        case OP_RVV_VSSE:
        case OP_RVV_VSUXEI:
        case OP_V128_STORE_STRIDED:
        case OP_V128_SCATTER:
        case OP_VSTORESS:
        case OP_VSTORESD:
        case OP_VSTORESS_RAW:
        case OP_STORE128:
        case OP_STORE64:
        case OP_MOV_MEM:
        /* Calls have external side effects */
        case OP_CALL:
        case OP_CALL_INDIRECT:
        case OP_CALL_ARG:
        case OP_SYSCALL:
        case OP_CLONE:
        /* Atomics, barriers, and traps */
        case OP_ATOMIC_XADD:
        case OP_ATOMIC_OR:
        case OP_ATOMIC_AND:
        case OP_ATOMIC_XOR:
        case OP_ATOMIC_CMPXCHG:
        case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG:
        case OP_MFENCE:
        case OP_TRAP:
        case OP_ASAN_CHECK:
        case OP_SETJMP:
        case OP_LONGJMP:
        case OP_NONLOCAL_GOTO:
        /* Inline assembly */
        case OP_ASM:
        case OP_ASM_ARG:
        case OP_ASM_LABEL:
        /* Stack and ABI frame operations */
        case OP_PUSH:
        case OP_POP:
        case OP_STACK_ALIGN:
        case OP_PARAM_SAVE_INT:
        case OP_PARAM_SAVE_FP:
        case OP_VARIADIC_SPILL:
        case OP_CALLEE_SAVE_STORES:
        case OP_CALLEE_SAVE_LOADS:
        case OP_VLA_ALLOC:
        case OP_RET:
        case OP_EPILOGUE:
        case OP_JMP:
        case OP_JMP_INDIRECT:
        case OP_JMP_LABEL:
            return true;
        default:
            break;
    }

    /* Return value registers at function boundary */
    if (instr->temp_dest == VREG_0 || instr->temp_dest == VREG_2 ||
        instr->temp_dest == VREG_F0 || instr->temp_dest == VREG_F1) {
        return true;
    }

    return false;
}

#define MAX_ADCE_USES 512

static void get_instruction_uses(ZeroCCCompiler *c, size_t idx, Instr *instr, int *uses, int *use_count) {
    *use_count = 0;
    if (!instr) return;

    if (instr->op == OP_CALL || instr->op == OP_CALL_INDIRECT || instr->op == OP_SYSCALL || instr->op == OP_CLONE) {
        int arg_count = instr->temp_dest;
        if (instr->op == OP_CALL_INDIRECT) {
            if (instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src1;
            if (arg_count > 0 && instr->temp_src2 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src2;
            if (c) {
                for (int a = 1; a < arg_count; a++) {
                    size_t ca_idx = idx + 1 + (size_t)(a - 1);
                    if (ca_idx >= ir_get_count(c)) break;
                    const Instr *ca_instr = ir_get(c, ca_idx);
                    if (!ca_instr || ca_instr->op != OP_CALL_ARG) break;
                    if (ca_instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)ca_instr->temp_src1;
                }
            }
        } else {
            if (arg_count > 0 && instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src1;
            if (arg_count > 1 && instr->temp_src2 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src2;
            if (c) {
                for (int a = 2; a < arg_count; a++) {
                    size_t ca_idx = idx + 1 + (size_t)(a - 2);
                    if (ca_idx >= ir_get_count(c)) break;
                    const Instr *ca_instr = ir_get(c, ca_idx);
                    if (!ca_instr || ca_instr->op != OP_CALL_ARG) break;
                    if (ca_instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)ca_instr->temp_src1;
                }
            }
        }
        return;
    }

    if (instr->op == OP_ASM) {
        int op_count = (int)instr->temp_dest;
        if (c) {
            for (int o = 0; o < op_count; o++) {
                const Instr *arg = ir_get(c, idx + 1 + (size_t)o);
                if (!arg || arg->op != OP_ASM_ARG) break;
                int t = (int)arg->temp_src1;
                if (t != (int)TEMP_NONE) {
                    int kind = (int)arg->cond;
                    if (kind == ZC_ASM_OP_IN_R || kind == ZC_ASM_OP_INOUT_R ||
                        kind == ZC_ASM_OP_IN_M || kind == ZC_ASM_OP_OUT_M) {
                        if (*use_count < MAX_ADCE_USES) uses[(*use_count)++] = t;
                    }
                }
            }
        }
        return;
    }

    if (instr->op == OP_ATOMIC_CMPXCHG || instr->op == OP_ATOMIC_CMPXCHG_BOOL || instr->op == OP_V128_SELECT) {
        if (instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src1;
        if (instr->temp_src2 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src2;
        if ((uint32_t)instr->imm64 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)(uint32_t)instr->imm64;
        return;
    }

    if (instr->temp_src1 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src1;
    if (instr->temp_src2 != TEMP_NONE && *use_count < MAX_ADCE_USES) uses[(*use_count)++] = (int)instr->temp_src2;
}

bool opt_adce(ZeroCCCompiler *c) {
    if (!c) return false;
    if (c->lowering.has_setjmp || c->lowering.has_capture_mirror || c->lowering.sp_snap_off != 0) return false;
    size_t n_instrs = ir_get_count(c);
    if (n_instrs <= 1) return false;

    if (!cfg_build(c)) return false;
    const int *pidom = cfg_post_idom(c);
    if (!pidom) return false;

    ZeroCCCFG *cfg = &c->cfg_cache;
    int nblocks = cfg->block_count;
    if (nblocks <= 0) return false;

    /* Map each instruction to its containing basic block */
    int *instr_to_block = (int *)malloc(n_instrs * sizeof(int));
    if (!instr_to_block) return false;
    for (int b = 0; b < nblocks; b++) {
        for (size_t i = cfg->blocks[b].start; i < cfg->blocks[b].end && i < n_instrs; i++) {
            instr_to_block[i] = b;
        }
    }

    /* Build Control Dependence Graph (CDG):
     * For each block x with multiple successors, a block y in the path from successor s
     * up to post_idom[x] is control-dependent on x. */
    CDList *cdg = (CDList *)calloc((size_t)nblocks, sizeof(CDList));
    if (!cdg) { free(instr_to_block); return false; }

    for (int x = 0; x < nblocks; x++) {
        if (cfg->blocks[x].succ_count > 1) {
            int x_pidom = pidom[x];
            for (int s_idx = 0; s_idx < cfg->blocks[x].succ_count; s_idx++) {
                int curr = cfg->blocks[x].succ[s_idx];
                int guard = 0;
                while (curr >= 0 && curr < nblocks && curr != x_pidom && guard++ < nblocks + 2) {
                    bool present = false;
                    for (int k = 0; k < cdg[curr].count; k++) {
                        if (cdg[curr].blocks[k] == x) { present = true; break; }
                    }
                    if (!present) {
                        if (cdg[curr].count >= cdg[curr].cap) {
                            int ncap = cdg[curr].cap ? cdg[curr].cap * 2 : 4;
                            int *nb = (int *)realloc(cdg[curr].blocks, (size_t)ncap * sizeof(int));
                            if (nb) {
                                cdg[curr].blocks = nb;
                                cdg[curr].cap = ncap;
                                cdg[curr].blocks[cdg[curr].count++] = x;
                            }
                        } else {
                            cdg[curr].blocks[cdg[curr].count++] = x;
                        }
                    }
                    if (curr == pidom[curr]) break;
                    curr = pidom[curr];
                }
            }
        }
    }

    /* Track definitions of each virtual/physical register */
    TempDefs *temp_defs = (TempDefs *)calloc(MAX_ADCE_VREGS, sizeof(TempDefs));
    if (!temp_defs) {
        for (int i = 0; i < nblocks; i++) free(cdg[i].blocks);
        free(cdg);
        free(instr_to_block);
        return false;
    }

    for (size_t i = 0; i < n_instrs; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins) continue;
        if (ins->temp_dest != TEMP_NONE && ins->op != OP_CALL && ins->op != OP_SYSCALL && ins->op != OP_CLONE) {
            uint32_t td = ins->temp_dest;
            if (td < MAX_ADCE_VREGS) {
                if (temp_defs[td].count >= temp_defs[td].cap) {
                    int ncap = temp_defs[td].cap ? temp_defs[td].cap * 2 : 2;
                    int *nd = (int *)realloc(temp_defs[td].defs, (size_t)ncap * sizeof(int));
                    if (nd) {
                        temp_defs[td].defs = nd;
                        temp_defs[td].cap = ncap;
                        temp_defs[td].defs[temp_defs[td].count++] = (int)i;
                    }
                } else {
                    temp_defs[td].defs[temp_defs[td].count++] = (int)i;
                }
            }
        }
    }

    /* Liveness arrays and worklist */
    bool *is_live = (bool *)calloc(n_instrs, sizeof(bool));
    bool *block_has_live = (bool *)calloc((size_t)nblocks, sizeof(bool));
    int *worklist = (int *)malloc(n_instrs * sizeof(int));
    int worklist_count = 0;

    if (!is_live || !block_has_live || !worklist) {
        free(is_live);
        free(block_has_live);
        free(worklist);
        for (int i = 0; i < MAX_ADCE_VREGS; i++) free(temp_defs[i].defs);
        free(temp_defs);
        for (int i = 0; i < nblocks; i++) free(cdg[i].blocks);
        free(cdg);
        free(instr_to_block);
        return false;
    }

    /* Step 1: Identify all Essential Roots */
    for (size_t i = 0; i < n_instrs; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && instr_is_adce_root(ins)) {
            is_live[i] = true;
            worklist[worklist_count++] = (int)i;
            int b = instr_to_block[i];
            if (b >= 0 && b < nblocks) block_has_live[b] = true;
        }
    }

    /* Step 2: Backward propagation worklist */
    int uses[MAX_ADCE_USES];
    int use_count = 0;

    while (worklist_count > 0) {
        int idx = worklist[--worklist_count];
        Instr *ins = ir_get(c, (size_t)idx);
        if (!ins) continue;

        int b = instr_to_block[idx];

        /* Propagate control dependencies: if block b has live instructions,
         * the branches in all blocks x that govern b must be marked live. */
        if (b >= 0 && b < nblocks) {
            for (int k = 0; k < cdg[b].count; k++) {
                int x = cdg[b].blocks[k];
                if (x >= 0 && x < nblocks) {
                    size_t term_idx = cfg->blocks[x].end - 1;
                    if (term_idx < n_instrs && !is_live[term_idx]) {
                        is_live[term_idx] = true;
                        worklist[worklist_count++] = (int)term_idx;
                        block_has_live[x] = true;

                        /* Also mark preceding flag-setting instruction (CMP) live */
                        for (int p = (int)term_idx - 1; p >= (int)cfg->blocks[x].start; p--) {
                            Instr *pin = ir_get(c, (size_t)p);
                            if (!pin) break;
                            if (pin->op == OP_CMP || pin->op == OP_CMP_ZERO || pin->flags_out == REG_FLAGS) {
                                if (!is_live[p]) {
                                    is_live[p] = true;
                                    worklist[worklist_count++] = p;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* Propagate data dependencies: mark all definitions of used temps live */
        get_instruction_uses(c, (size_t)idx, ins, uses, &use_count);
        for (int u = 0; u < use_count; u++) {
            int t = uses[u];
            if (t >= 0 && t < MAX_ADCE_VREGS) {
                for (int d = 0; d < temp_defs[t].count; d++) {
                    int def_i = temp_defs[t].defs[d];
                    if (def_i >= 0 && (size_t)def_i < n_instrs && !is_live[def_i]) {
                        is_live[def_i] = true;
                        worklist[worklist_count++] = def_i;
                        int db = instr_to_block[def_i];
                        if (db >= 0 && db < nblocks) block_has_live[db] = true;
                    }
                }
            }
        }

        /* If this instruction reads flags (e.g. COND_SET or flag-consuming branch),
         * mark the preceding flag-setting instruction (CMP) in the same block live. */
        if (ins->flags_in == REG_FLAGS || ins->op == OP_COND_SET ||
            ins->op == OP_JE || ins->op == OP_JNE || ins->op == OP_JL ||
            ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
            for (int p = idx - 1; p >= (int)cfg->blocks[b].start; p--) {
                Instr *pin = ir_get(c, (size_t)p);
                if (!pin) break;
                if (pin->flags_out == REG_FLAGS || pin->op == OP_CMP ||
                    pin->op == OP_CMP_ZERO || pin->op == OP_CMP128) {
                    if (!is_live[p]) {
                        is_live[p] = true;
                        worklist[worklist_count++] = p;
                        int pb = instr_to_block[p];
                        if (pb >= 0 && pb < nblocks) block_has_live[pb] = true;
                    }
                    break;
                }
            }
        }
    }

    /* Step 3: Dead branch redirection and dead instruction elimination */
    bool changed = false;

    for (int b = 0; b < nblocks; b++) {
        if (cfg->blocks[b].succ_count > 1) {
            size_t term_idx = cfg->blocks[b].end - 1;
            if (term_idx < n_instrs && !is_live[term_idx]) {
                Instr *term = ir_get(c, term_idx);
                int p = pidom[b];
                if (p >= 0 && p < nblocks && p != b) {
                    Instr *p_first = ir_get(c, cfg->blocks[p].start);
                    if (p_first && p_first->op == OP_LABEL) {
                        term->op = OP_JMP;
                        term->temp_dest = TEMP_NONE;
                        term->temp_src1 = TEMP_NONE;
                        term->temp_src2 = TEMP_NONE;
                        term->imm = 0;
                        term->imm64 = 0;
                        term->label_idx = p_first->label_idx;
                        is_live[term_idx] = true;
                        changed = true;
                    } else if (p == b + 1) {
                        term->op = OP_DEAD;
                        changed = true;
                    }
                }
            }
        }
    }

    /* Mark all non-live instructions as OP_DEAD */
    for (size_t i = 0; i < n_instrs; i++) {
        if (!is_live[i]) {
            Instr *ins = ir_get(c, i);
            if (ins && ins->op != OP_LABEL && ins->op != OP_EPILOGUE && ins->op != OP_DEAD) {
                ins->op = OP_DEAD;
                changed = true;
            }
        }
    }

    /* Step 4: Compact IR */
    if (changed) {
        size_t write_idx = 0;
        for (size_t i = 0; i < n_instrs; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            if (ins->op != OP_DEAD) {
                if (write_idx != i) {
                    Instr *dst = ir_get(c, write_idx);
                    *dst = *ins;
                }
                write_idx++;
            }
        }
        c->ir_arena.count = write_idx;
        cfg_invalidate(c);
    }

    /* Clean up all temporary structures */
    free(is_live);
    free(block_has_live);
    free(worklist);
    for (int i = 0; i < MAX_ADCE_VREGS; i++) {
        if (temp_defs[i].defs) free(temp_defs[i].defs);
    }
    free(temp_defs);
    for (int i = 0; i < nblocks; i++) {
        if (cdg[i].blocks) free(cdg[i].blocks);
    }
    free(cdg);
    free(instr_to_block);

    return changed;
}
