#ifndef ZERO_CC_PARSER_LIVE_RANGE_H
#define ZERO_CC_PARSER_LIVE_RANGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"

/* Forward declarations to avoid circular includes */
typedef struct RegAllocator RegAllocator;

/*
 * Live Range Analysis for Two-Pass Code Generation
 *
 * Computes live intervals for temporary variables from IR instructions.
 * Used by the linear-scan register allocator to assign physical registers.
 */

/* Track definition and use of temporaries */
typedef struct TempUse {
    size_t instr_index;     /* Instruction index where temp is used/defined */
    int temp_id;            /* Temporary variable ID */
    bool is_def;            /* true = definition, false = use */
} TempUse;

/* Live range analysis state */
typedef struct LiveRangeAnalyzer {
    TempUse *uses;              /* Array of temp uses/defs */
    size_t use_count;           /* Number of uses */
    size_t use_capacity;        /* Capacity of uses array */

    /* Live intervals per temp */
    size_t *temp_def_pos;       /* Definition position per temp */
    size_t *temp_live_end;      /* Last use position per temp */
    RegClass *temp_reg_class;   /* Register class per temp (GPR or XMM) */
    int *param_slot;            /* Sprint 117: ABI int-arg slot register for
                                 * OP_PARAM_LOAD temps (-1 = not a parameter) */
    size_t temp_capacity;       /* Capacity of temp arrays */

    /* Clobber tracking per instruction */
    bool *rax_clobbered;        /* True if rax is clobbered at instruction i */
    bool *rdx_clobbered;        /* True if rdx is clobbered at instruction i */
    size_t clobber_capacity;    /* Capacity of clobber arrays */

    /* OP_CALL instruction positions */
    size_t *call_positions;     /* Indices of OP_CALL instructions */
    size_t call_count;          /* Number of calls */
    size_t call_capacity;       /* Capacity of call_positions array */
} LiveRangeAnalyzer;

/* Initialize live range analyzer */
static void live_range_init(LiveRangeAnalyzer *lra) __attribute__((unused));
static void live_range_init(LiveRangeAnalyzer *lra) {
    if (!lra) return;

    lra->uses = NULL;
    lra->use_count = 0;
    lra->use_capacity = 0;

    lra->temp_def_pos = NULL;
    lra->temp_live_end = NULL;
    lra->temp_reg_class = NULL;
    lra->param_slot = NULL;
    lra->temp_capacity = 0;

    lra->rax_clobbered = NULL;
    lra->rdx_clobbered = NULL;
    lra->clobber_capacity = 0;

    lra->call_positions = NULL;
    lra->call_count = 0;
    lra->call_capacity = 0;
}

/* Free live range analyzer */
static void live_range_free(LiveRangeAnalyzer *lra) __attribute__((unused));
static void live_range_free(LiveRangeAnalyzer *lra) {
    if (!lra) return;

    free(lra->uses);
    free(lra->temp_def_pos);
    free(lra->temp_live_end);
    free(lra->temp_reg_class);
    free(lra->param_slot);
    free(lra->rax_clobbered);
    free(lra->rdx_clobbered);
    free(lra->call_positions);

    lra->uses = NULL;
    lra->temp_def_pos = NULL;
    lra->temp_live_end = NULL;
    lra->temp_reg_class = NULL;
    lra->rax_clobbered = NULL;
    lra->rdx_clobbered = NULL;
    lra->call_positions = NULL;
    lra->call_count = 0;
    lra->call_capacity = 0;
}

/* Mark a register as clobbered at a given instruction */
static void live_range_mark_clobber(LiveRangeAnalyzer *lra, size_t instr_index, int reg_id) __attribute__((unused));
static void live_range_mark_clobber(LiveRangeAnalyzer *lra, size_t instr_index, int reg_id) {
    if (!lra) return;

    /* Expand clobber arrays if needed */
    if (instr_index >= lra->clobber_capacity) {
        size_t new_cap = lra->clobber_capacity ? lra->clobber_capacity * 2 : 128;
        while (new_cap <= instr_index) new_cap *= 2;

        bool *new_rax = realloc(lra->rax_clobbered, new_cap * sizeof(bool));
        bool *new_rdx = realloc(lra->rdx_clobbered, new_cap * sizeof(bool));
        if (!new_rax || !new_rdx) return;

        lra->rax_clobbered = new_rax;
        lra->rdx_clobbered = new_rdx;
        lra->clobber_capacity = new_cap;

        /* Initialize new entries to false */
        for (size_t i = lra->clobber_capacity / 2; i < new_cap; i++) {
            lra->rax_clobbered[i] = false;
            lra->rdx_clobbered[i] = false;
        }
    }

    /* Mark the register as clobbered */
    if (reg_id == 0) {  /* rax */
        lra->rax_clobbered[instr_index] = true;
    } else if (reg_id == 2) {  /* rdx */
        lra->rdx_clobbered[instr_index] = true;
    }
}

/* Add a temp use/def */
static void live_range_add_use(LiveRangeAnalyzer *lra, size_t instr_index, int temp_id, bool is_def) __attribute__((unused));
static void live_range_add_use(LiveRangeAnalyzer *lra, size_t instr_index, int temp_id, bool is_def) {
    /* C2b: temp ids are 16-bit (up to 65534); the per-temp arrays below
     * realloc-grow to fit, so no fixed cap. */
    if (!lra || temp_id < 0) return;

    /* Expand uses array if needed */
    if (lra->use_count >= lra->use_capacity) {
        size_t new_cap = lra->use_capacity ? lra->use_capacity * 2 : 64;
        TempUse *new_uses = realloc(lra->uses, new_cap * sizeof(TempUse));
        if (!new_uses) return;
        lra->uses = new_uses;
        lra->use_capacity = new_cap;
    }

    TempUse *use = &lra->uses[lra->use_count++];
    use->instr_index = instr_index;
    use->temp_id = temp_id;
    use->is_def = is_def;

    /* Expand temp arrays if needed */
    if ((size_t)temp_id >= lra->temp_capacity) {
        size_t new_cap = lra->temp_capacity ? lra->temp_capacity * 2 : 256;
        size_t new_temp = (size_t)temp_id + 1;
        if (new_cap < new_temp) new_cap = new_temp;

        size_t *new_def_pos = realloc(lra->temp_def_pos, new_cap * sizeof(size_t));
        if (!new_def_pos) return;

        size_t *new_live_end = realloc(lra->temp_live_end, new_cap * sizeof(size_t));
        if (!new_live_end) {
            free(new_def_pos);
            return;
        }

        RegClass *new_reg_class = realloc(lra->temp_reg_class, new_cap * sizeof(RegClass));
        if (!new_reg_class) {
            free(new_def_pos);
            free(new_live_end);
            return;
        }

        int *new_param_slot = realloc(lra->param_slot, new_cap * sizeof(int));
        if (!new_param_slot) {
            free(new_def_pos);
            free(new_live_end);
            free(new_reg_class);
            return;
        }

        /* Initialize new entries */
        for (size_t i = lra->temp_capacity; i < new_cap; i++) {
            new_def_pos[i] = SIZE_MAX;  /* Undefined */
            new_live_end[i] = 0;
            new_reg_class[i] = REG_CLASS_GPR;  /* Default to GPR */
            new_param_slot[i] = -1;  /* Not a parameter */
        }

        lra->temp_def_pos = new_def_pos;
        lra->temp_live_end = new_live_end;
        lra->temp_reg_class = new_reg_class;
        lra->param_slot = new_param_slot;
        lra->temp_capacity = new_cap;
    }

    /* Track definition position (first def) */
    if (is_def && lra->temp_def_pos[temp_id] == SIZE_MAX) {
        lra->temp_def_pos[temp_id] = instr_index;
    }

    /* Update live end (last use) */
    if (instr_index > lra->temp_live_end[temp_id]) {
        lra->temp_live_end[temp_id] = instr_index;
    }
}

/* Compute live intervals from IR and add to register allocator */
static void live_range_compute(LiveRangeAnalyzer *lra, RegAllocator *ra, ZeroCCCompiler *c) __attribute__((unused));
static void live_range_compute(LiveRangeAnalyzer *lra, RegAllocator *ra, ZeroCCCompiler *c) {
    if (!lra || !ra) return;

    /* Detect loops to extend live intervals across backedges:
     * If a temp is defined before or within a loop and used within the loop,
     * its live interval must extend to at least the loop backedge (exit_idx)
     * so that its assigned physical register is preserved across loop iterations! */
    size_t instr_count = ir_get_count(c);
    int max_label = -1;
    for (size_t i = 0; i < instr_count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && (int)ins->label_idx > max_label) {
            max_label = (int)ins->label_idx;
        }
    }
    int *label_to_pos = NULL;
    if (max_label >= 0) {
        label_to_pos = (int *)malloc((max_label + 1) * sizeof(int));
        if (label_to_pos) {
            for (int i = 0; i <= max_label; i++) label_to_pos[i] = -1;
            for (size_t i = 0; i < instr_count; i++) {
                Instr *ins = ir_get(c, i);
                if (ins && ins->op == OP_LABEL) label_to_pos[ins->label_idx] = (int)i;
            }
        }
    }

    /* RISC-V flag liveness: RISC-V has no flags register. The backend
     * re-materializes a comparison predicate from the CMP/CMP_ZERO operand
     * registers at the consumer (OP_COND_SET / OP_JCOND), so those operand
     * intervals must extend from the flag producer to the consumer (or to
     * the next flag producer / control-flow break, whichever comes first).
     * On ISAs with real flags (x86-64, AArch64) the EFLAGS/NZCV state is
     * independent of register allocation, so no extension is needed. */
    if (ra->target_isa == ISA_RISCV64 || ra->target_isa == ISA_RISCV32) {
        size_t *flag_end = (size_t *)malloc(instr_count * sizeof(size_t));
        if (flag_end) {
            for (size_t i = 0; i < instr_count; i++) flag_end[i] = i;
            for (size_t i = 0; i < instr_count; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins || ins->flags_out != REG_FLAGS) continue;
                for (size_t j = i + 1; j < instr_count; j++) {
                    Instr *nxt = ir_get(c, j);
                    if (!nxt) break;
                    if (nxt->flags_in == REG_FLAGS) { flag_end[i] = j; break; }
                    if (nxt->flags_out == REG_FLAGS) { flag_end[i] = j - 1; break; }
                    if (nxt->op == OP_LABEL || nxt->op == OP_JMP || nxt->op == OP_JMP_LABEL ||
                        nxt->op == OP_JE || nxt->op == OP_JNE || nxt->op == OP_JL ||
                        nxt->op == OP_JLE || nxt->op == OP_JG || nxt->op == OP_JCOND ||
                        nxt->op == OP_CALL || nxt->op == OP_CALL_INDIRECT ||
                        nxt->op == OP_SETJMP || nxt->op == OP_LONGJMP ||
                        nxt->op == OP_RET || nxt->op == OP_EPILOGUE) {
                        flag_end[i] = j - 1; break;
                    }
                }
            }
            for (size_t i = 0; i < instr_count; i++) {
                Instr *ins = ir_get(c, i);
                if (!ins || ins->flags_out != REG_FLAGS) continue;
                int operands[2] = { (int)ins->temp_src1, (int)ins->temp_src2 };
                for (int k = 0; k < 2; k++) {
                    int t = operands[k];
                    if (t == TEMP_NONE || (size_t)t >= lra->temp_capacity) continue;
                    if (lra->temp_def_pos[t] == SIZE_MAX) continue;
                    if (flag_end[i] > lra->temp_live_end[t]) lra->temp_live_end[t] = flag_end[i];
                }
            }
            free(flag_end);
        }
    }

    /* Pre-collect loop backedges (hdr -> backedge_pos) once before the
     * per-temp loop, avoiding an O(temps * instrs) scan over the IR buffer. */
    typedef struct {
        size_t hdr;
        size_t backedge;
    } LRABackedge;
    LRABackedge *backedges = NULL;
    size_t backedge_count = 0;
    size_t backedge_cap = 0;
    if (label_to_pos) {
        for (size_t i = 0; i < instr_count; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            if (ins->op == OP_JMP || ins->op == OP_JE || ins->op == OP_JNE ||
                ins->op == OP_JL || ins->op == OP_JLE || ins->op == OP_JG || ins->op == OP_JCOND) {
                int lbl = (int)ins->label_idx;
                if (lbl >= 0 && lbl <= max_label) {
                    int hdr = label_to_pos[lbl];
                    if (hdr >= 0 && (size_t)hdr < i) {
                        if (backedge_count >= backedge_cap) {
                            size_t ncap = backedge_cap ? backedge_cap * 2 : 32;
                            LRABackedge *narr = (LRABackedge *)realloc(backedges, ncap * sizeof(LRABackedge));
                            if (!narr) break;
                            backedges = narr;
                            backedge_cap = ncap;
                        }
                        backedges[backedge_count].hdr = (size_t)hdr;
                        backedges[backedge_count].backedge = i;
                        backedge_count++;
                    }
                }
            } else if (ins->op == OP_JMP_INDIRECT) {
                /* Indirect jump can jump back to any previous label in the function. */
                for (int lbl = 0; lbl <= max_label; lbl++) {
                    int hdr = label_to_pos[lbl];
                    if (hdr >= 0 && (size_t)hdr < i) {
                        if (backedge_count >= backedge_cap) {
                            size_t ncap = backedge_cap ? backedge_cap * 2 : 32;
                            LRABackedge *narr = (LRABackedge *)realloc(backedges, ncap * sizeof(LRABackedge));
                            if (!narr) break;
                            backedges = narr;
                            backedge_cap = ncap;
                        }
                        backedges[backedge_count].hdr = (size_t)hdr;
                        backedges[backedge_count].backedge = i;
                        backedge_count++;
                    }
                }
            }
        }
    }

    /* On x86-64, idiv/div and 128-bit multiply implicitly read/write RAX (phys 0)
     * and RDX (phys 2). Register intervals at [i, i] for VREG_0 and VREG_2 so that
     * the register allocator never assigns RAX or RDX to any temp that is live
     * across or used at an OP_DIV, OP_MOD, or OP_MUL128 (the 128-bit mul uses
     * rdx:rax as its working product, so a live 64-bit value in rax/rdx would be
     * silently clobbered -- Sprint 109 GPR128 spill fix).
     * NOTE: these fixed-interval registrations must run BEFORE the per-temp loop
     * below so the Sprint 117 parameter pre-coloring clash check can see them
     * (an x86 int param #3 lives in RDX = VREG_2's pre-colored register). */
    if (ra->target_isa == ISA_X86_64) {
        for (size_t i = 0; i < instr_count; i++) {
            Instr *ins = ir_get(c, i);
            if (ins && (ins->op == OP_DIV || ins->op == OP_MOD || ins->op == OP_MUL128)) {
                regalloc_add_interval(ra, VREG_0, i, i, REG_CLASS_GPR);
                regalloc_add_interval(ra, VREG_2, i, i, REG_CLASS_GPR);
            }
        }
    }

    /* Protect return registers across post-call reads (Sprint 109 return fix):
     * A call returning a 128-bit value or 9-16B struct produces result halves in
     * VREG_0 and VREG_2 (rdx on x86, x1 on aarch64, a1 on riscv).  If the caller
     * reads them in subsequent instructions (e.g. lo = VREG_0; hi = VREG_2), the
     * allocator must not assign the high return register to `lo` or any other temp
     * live during the copy window, which would overwrite the high half before it
     * is read. */
    for (size_t i = 0; i < instr_count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && (ins->op == OP_CALL || ins->op == OP_SYSCALL || ins->op == OP_CLONE)) {
            size_t end0 = i, end2 = i;
            for (size_t k = i + 1; k < instr_count && k <= i + 6; k++) {
                Instr *nxt = ir_get(c, k);
                if (!nxt) break;
                if (nxt->op == OP_CALL || nxt->op == OP_SYSCALL || nxt->op == OP_CLONE) break;
                if (nxt->temp_src1 == VREG_0 || nxt->temp_src2 == VREG_0) end0 = k;
                if (nxt->temp_src1 == VREG_2 || nxt->temp_src2 == VREG_2) end2 = k;
            }
            if (end0 > i) regalloc_add_interval(ra, VREG_0, i, end0, REG_CLASS_GPR);
            if (end2 > i) regalloc_add_interval(ra, VREG_2, i, end2, REG_CLASS_GPR);
        }
    }

    /* Process each temp that was defined */
    for (size_t temp_id = 0; temp_id < lra->temp_capacity; temp_id++) {
        if (lra->temp_def_pos[temp_id] == SIZE_MAX) {
            continue;  /* Temp was never defined */
        }

        int param_slot = (lra->param_slot && lra->param_slot[temp_id] >= 0)
                         ? lra->param_slot[temp_id] : -1;
        size_t start = lra->temp_def_pos[temp_id];
        size_t end = lra->temp_live_end[temp_id];
        if (param_slot >= 0) {
            /* Sprint 117: the ABI argument value already sits in its slot
             * register at function entry — liveness starts there, not at the
             * OP_PARAM_LOAD bookkeeping instruction. */
            start = 0;
        }

        /* Extend end position across any loop that contains uses of this temp.
         * Multiple nested loops may require fixed-point propagation (e.g. a
         * constant hoisted before an outer loop that is only directly used inside
         * an inner loop: first extends to the inner backedge, which is inside the
         * outer loop, and then extends to the outer backedge). */
        if (backedge_count > 0) {
            bool changed = true;
            while (changed) {
                changed = false;
                for (size_t b = 0; b < backedge_count; b++) {
                    size_t hdr = backedges[b].hdr;
                    size_t i = backedges[b].backedge;
                    if (start < hdr && end >= hdr && end < i) {
                        end = i;
                        changed = true;
                    }
                }
            }
        }

        /* Skip if start > end (shouldn't happen, but be safe) */
        if (start > end) continue;

        /* Determine register class from the defining instruction's opcode */
        RegClass reg_class = lra->temp_reg_class[temp_id];

        /* Does the value live across a call? For parameters `start` is the
         * entry-extended position (a caller-saved slot is clobbered by any
         * call in the window). */
        bool spans_call = false;
        if (lra->call_count > 0) {
            for (size_t c = 0; c < lra->call_count; c++) {
                size_t call_pos = lra->call_positions[c];
                if (start < call_pos && end > call_pos) {
                    spans_call = true;
                    break;
                }
            }
        }

        /* Sprint 117 clash check: no other fixed interval (a VREG_0/VREG_2
         * return/scratch guard, or an earlier parameter's slot) may overlap
         * this parameter's window on the same register. If one does, the
         * value cannot be pre-colored to the slot for the whole window. */
        bool clash = false;
        if (param_slot >= 0) {
            for (size_t k = 0; k < ra->fixed_interval_count; k++) {
                const LiveInterval *other = &ra->intervals[ra->fixed_interval_indices[k]];
                if (other->reg_class != REG_CLASS_GPR) continue;
                if (regalloc_fixed_reg_of(ra, other) != param_slot) continue;
                if (!(end < other->start || start > other->end)) {
                    clash = true;
                    break;
                }
            }
        }

        /* Add interval to register allocator */
        int int_idx = regalloc_add_interval(ra, (int)temp_id, start, end, reg_class);

        if (int_idx >= 0 && spans_call) {
            regalloc_set_interval_spans_call(ra, int_idx, true);
        }

        if (param_slot >= 0 && int_idx >= 0) {
            if (!spans_call && !clash) {
                /* Pre-color the whole interval to the ABI slot: no
                 * materialization move is needed, and the conflict check keeps
                 * every other interval off the slot during the live window. */
                regalloc_set_fixed_reg(ra, int_idx, param_slot);
            } else {
                /* The value sits in the ABI slot from entry until the
                 * OP_PARAM_LOAD moves it into the allocated register; reserve
                 * the slot for that window only. */
                int guard_idx = regalloc_add_interval(ra, (int)temp_id, 0,
                                                      lra->temp_def_pos[temp_id],
                                                      reg_class);
                if (guard_idx >= 0) {
                    regalloc_set_fixed_reg(ra, guard_idx, param_slot);
                }
            }
        }

        /* Sprint 135: inline-asm operand temps are no_spill (the template
         * references them at the OP_ASM site); a fixed-register constraint
         * (a/b/c/d/S/D) pre-colors the interval like an ABI parameter slot.
         * Registers the template clobbers are forbidden for the operand. */
        if (c && c->asm_operand_count > 0 && int_idx >= 0) {
            for (size_t k = 0; k < c->asm_operand_count; k++) {
                if (c->asm_operand_temps[k] != (int)temp_id) continue;
                regalloc_set_no_spill(ra, int_idx);
                if (c->asm_fixed_regs && c->asm_fixed_regs[k] >= 0) {
                    regalloc_set_fixed_reg(ra, int_idx, c->asm_fixed_regs[k]);
                }
                /* Find this operand's OP_ASM and forbid its clobbered regs */
                for (size_t a = 0; a < c->ir_asm_args.count; a++) {
                    const IRAsmArgs *aa = &c->ir_asm_args.table[a];
                    for (int o = 0; o < aa->op_count; o++) {
                        if (aa->ops[o].temp != (uint32_t)temp_id) continue;
                        if (aa->clobber_mask) {
                            regalloc_set_forbidden(ra, int_idx, aa->clobber_mask);
                        }
                        break;
                    }
                }
                break;
            }
        }
    }

    if (backedges) free(backedges);
    if (label_to_pos) free(label_to_pos);
}

/* Record a call position in the analyzer */
static void live_range_record_call(LiveRangeAnalyzer *lra, size_t instr_index) __attribute__((unused));
static void live_range_record_call(LiveRangeAnalyzer *lra, size_t instr_index) {
    if (!lra) return;
    if (lra->call_count >= lra->call_capacity) {
        size_t new_cap = lra->call_capacity ? lra->call_capacity * 2 : 16;
        size_t *new_arr = realloc(lra->call_positions, new_cap * sizeof(size_t));
        if (!new_arr) return;
        lra->call_positions = new_arr;
        lra->call_capacity = new_cap;
    }
    lra->call_positions[lra->call_count++] = instr_index;
}

/* Scan IR and record temp uses/defs */
static void live_range_scan_ir(LiveRangeAnalyzer *lra, const Instr *instrs, size_t instr_count, ZeroCCCompiler *c) __attribute__((unused));
static void live_range_scan_ir(LiveRangeAnalyzer *lra, const Instr *instrs, size_t instr_count, ZeroCCCompiler *c) {
    if (!lra || !instrs) return;

    for (size_t i = 0; i < instr_count; i++) {
        const Instr *instr = &instrs[i];

        /* Skip labels, dead/nop instructions, prologue, and unconditional jumps */
        if (instr->op == OP_LABEL || instr->op == OP_JMP || instr->op == OP_DEAD || instr->op == OP_NOP ||
            instr->op == OP_PARAM_SAVE_INT || instr->op == OP_PARAM_SAVE_FP || instr->op == OP_VARIADIC_SPILL) {
            continue;
        }

        /* OP_CALL packs argument temps in temp_src1, temp_src2, and imm (temp_dest is arg_count);
         * the complete list (5+ args) lives in the IR call-args side table.
         * OP_SYSCALL uses the identical packing - it clobbers a subset of what
         * a call clobers, so it is recorded as a call position (spans_call
         * barrier) with the same side-table argument liveness. */
        /* OP_CALL_ARG pseudo-instructions are part of the preceding OP_CALL/
         * OP_SYSCALL/OP_CLONE; they are skipped here and handled when that
         * call instruction is processed. */
        if (instr->op == OP_CALL_ARG) {
            continue;
        }

        if (instr->op == OP_SETJMP) {
            /* OP_SETJMP is a returns_twice boundary: any temp live across it
             * must be treated as call-spanning so it is never placed in a
             * caller-saved register clobbered before longjmp resumes here. */
            live_range_record_call(lra, i);
        }

        if (instr->op == OP_CALL || instr->op == OP_SYSCALL || instr->op == OP_CLONE) {
            live_range_record_call(lra, i);
            int arg_count = (int)instr->temp_dest;
            /* Sprint 151c: args 0/1 in temp_src1/temp_src2 with is_fp in
             * cond/width; args 2+ in OP_CALL_ARG pseudo-instructions. */
            if (arg_count > 0 && instr->temp_src1 != TEMP_NONE) {
                live_range_add_use(lra, i, instr->temp_src1, false);
            }
            if (arg_count > 1 && instr->temp_src2 != TEMP_NONE) {
                live_range_add_use(lra, i, instr->temp_src2, false);
            }
            for (int a = 2; a < arg_count; a++) {
                size_t ca_idx = i + 1 + (a - 2);
                if (ca_idx >= (size_t)instr_count) break;
                const Instr *ca_instr = &instrs[ca_idx];
                if (ca_instr->op != OP_CALL_ARG) break;
                live_range_add_use(lra, i, ca_instr->temp_src1, false);
            }
            continue;
        }

        /* OP_ASM (Sprint 135): operand liveness lives in the IRAsmArgs side
         * table. An output r / in-out operand is DEFINED by the asm; an
         * input r operand and every m address temp is USED. An in-out temp
         * is both (same position, like an in-place op) so its pre-asm value
         * (the lvalue load the lowering emits) stays live in the operand
         * register across the template. */
        if (instr->op == OP_ASM) {
            const IRAsmArgs *aa = ir_asm_args_find(c, i);
            if (aa) {
                for (int o = 0; o < aa->op_count; o++) {
                    const IRAsmOperand *opn = &aa->ops[o];
                    if (opn->temp < 0) continue;
                    if (opn->kind == ZC_ASM_OP_OUT_R || opn->kind == ZC_ASM_OP_INOUT_R) {
                        live_range_add_use(lra, i, opn->temp, true);
                    }
                    if (opn->kind == ZC_ASM_OP_IN_R || opn->kind == ZC_ASM_OP_INOUT_R ||
                        opn->kind == ZC_ASM_OP_OUT_M || opn->kind == ZC_ASM_OP_IN_M) {
                        live_range_add_use(lra, i, opn->temp, false);
                    }
                }
            }
            continue;
        }

        /* Record destination temp (definition) - skip 0xFF (no temp) */
        if (instr->temp_dest!= TEMP_NONE) {
            live_range_add_use(lra, i, instr->temp_dest, true);
            /* Set register class based on the defining instruction's opcode */
            if ((size_t)instr->temp_dest < lra->temp_capacity) {
                if (instr->op == OP_CVTTSD2SI) {
                    lra->temp_reg_class[instr->temp_dest] = REG_CLASS_GPR;
                } else if (instr->op == OP_PARAM_LOAD) {
                    lra->temp_reg_class[instr->temp_dest] = instr->cond ? REG_CLASS_XMM : REG_CLASS_GPR;
                    if (instr->cond == 0) {
                        /* Sprint 117: record the ABI int-arg slot so the
                         * per-temp loop can pre-color the parameter interval
                         * to it (the value is live in that register from
                         * function entry). FP params keep the old behavior. */
                        const ISACallingConvention *conv = BE(c)->conv;
                        int ai = (int)instr->imm;
                        lra->param_slot[instr->temp_dest] =
                            (conv && ai >= 0 && ai < conv->int_arg_reg_count)
                            ? conv->int_arg_regs[ai] : -1;
                    }
                } else if (ir_opcode_is_float(instr->op)) {
                    lra->temp_reg_class[instr->temp_dest] = REG_CLASS_XMM;
                } else if (ir_opcode_is_simd(instr->op)) {
                    /* SIMD instructions produce vector register results */
                    lra->temp_reg_class[instr->temp_dest] = REG_CLASS_VEC;
                } else if (instr->op == OP_ADD128 || instr->op == OP_SUB128 || instr->op == OP_MUL128 ||
                    instr->op == OP_AND128 || instr->op == OP_OR128 || instr->op == OP_XOR128 ||
                    instr->op == OP_NOT128 || instr->op == OP_NEG128 || instr->op == OP_SHL128 ||
                    instr->op == OP_SHR128 || instr->op == OP_SAR128 || instr->op == OP_MOV128 ||
                    instr->op == OP_LOAD128 || instr->op == OP_ZEXT64_128 || instr->op == OP_SEXT64_128 ||
                    instr->op == OP_MOV128_IMM || instr->op == OP_MK128) {
                    lra->temp_reg_class[instr->temp_dest] = REG_CLASS_GPR128;
                }
            }
        }

        /* Record source temps (uses) - skip 0xFF (no temp) */
        if (instr->temp_src1!= TEMP_NONE) {
            live_range_add_use(lra, i, instr->temp_src1, false);
            if ((size_t)instr->temp_src1 < lra->temp_capacity) {
                if (instr->op == OP_CVTSI2SD || instr->op == OP_VCVT_SI2SS || instr->op == OP_VCVT_SI2SD ||
                    instr->op == OP_VLOAD || instr->op == OP_VSTORE ||
                    instr->op == OP_VLOADSS || instr->op == OP_VLOADSD ||
                    instr->op == OP_VSTORESS || instr->op == OP_VSTORESD) {
                    lra->temp_reg_class[instr->temp_src1] = REG_CLASS_GPR;
                } else if (instr->op == OP_CVTTSD2SI || instr->op == OP_FSTORE || instr->op == OP_FMOV ||
                    instr->op == OP_CVTSS2SD || instr->op == OP_CVTSD2SS) {
                    lra->temp_reg_class[instr->temp_src1] = REG_CLASS_XMM;
                } else if (ir_opcode_is_simd(instr->op)) {
                    /* SIMD instructions consume vector registers for sources */
                    lra->temp_reg_class[instr->temp_src1] = REG_CLASS_VEC;
                } else if (instr->op == OP_ADD128 || instr->op == OP_SUB128 || instr->op == OP_MUL128 ||
                    instr->op == OP_AND128 || instr->op == OP_OR128 || instr->op == OP_XOR128 ||
                    instr->op == OP_NOT128 || instr->op == OP_NEG128 || instr->op == OP_SHL128 ||
                    instr->op == OP_SHR128 || instr->op == OP_SAR128 || instr->op == OP_CMP128 ||
                    instr->op == OP_MOV128 || instr->op == OP_STORE128 || instr->op == OP_TRUNC128_64 ||
                    instr->op == OP_LO128 || instr->op == OP_HI128) {
                    lra->temp_reg_class[instr->temp_src1] = REG_CLASS_GPR128;
                }
            }
        }
        if (instr->temp_src2!= TEMP_NONE) {
            live_range_add_use(lra, i, instr->temp_src2, false);
            if ((size_t)instr->temp_src2 < lra->temp_capacity) {
                if (instr->op == OP_FSTORE) {
                    lra->temp_reg_class[instr->temp_src2] = REG_CLASS_GPR; /* base pointer */
                } else if (instr->op == OP_VSTORE) {
                    lra->temp_reg_class[instr->temp_src2] = REG_CLASS_VEC; /* vector value to store */
                } else if (instr->op == OP_FADD || instr->op == OP_FSUB || instr->op == OP_FMUL || instr->op == OP_FDIV) {
                    lra->temp_reg_class[instr->temp_src2] = REG_CLASS_XMM;
                } else if (ir_opcode_is_simd(instr->op)) {
                    /* SIMD instructions consume vector registers for sources */
                    lra->temp_reg_class[instr->temp_src2] = REG_CLASS_VEC;
                } else if (instr->op == OP_ADD128 || instr->op == OP_SUB128 || instr->op == OP_MUL128 ||
                    instr->op == OP_AND128 || instr->op == OP_OR128 || instr->op == OP_XOR128 ||
                    instr->op == OP_CMP128) {
                    lra->temp_reg_class[instr->temp_src2] = REG_CLASS_GPR128;
                }
            }
        }

        /* DIV/MOD clobbers rax and rdx - extend live ranges of temps 0 and 2
         * Temp 0 is typically assigned to rax, temp 2 to rdx.
         * Extending their live ranges to the DIV instruction reserves
         * these registers for the DIV and prevents other temps from using them.
         * Also mark rax and rdx as clobbered so the register allocator knows
         * not to assign other temps to these registers during DIV. */
        if (instr->op == OP_DIV || instr->op == OP_MOD || instr->op == OP_MUL128) {
            /* Extend rax (temp 0) live range to DIV instruction */
            live_range_add_use(lra, i, 0, false);  /* rax use at DIV */
            /* Extend rdx (temp 2) live range to DIV instruction */
            live_range_add_use(lra, i, 2, false);  /* rdx use at DIV */
            /* Mark rax and rdx as clobbered at instruction i */
            live_range_mark_clobber(lra, i, 0);  /* rax clobbered */
            live_range_mark_clobber(lra, i, 2);  /* rdx clobbered */
        }

        if (instr->op == OP_ATOMIC_CMPXCHG || instr->op == OP_ATOMIC_CMPXCHG_BOOL) {
            if (instr->label_idx != TEMP_NONE) {
                live_range_add_use(lra, i, instr->label_idx, false);
            }
            live_range_add_use(lra, i, 0, false);
            live_range_mark_clobber(lra, i, 0);
        }
    }
}

/* Get register name for a register index and class */
static const char *live_range_get_reg_name(RegClass reg_class, int reg_index) __attribute__((unused));
static const char *live_range_get_reg_name(RegClass reg_class, int reg_index) {
    static const char *gpr_names[] = {"rax", "rcx", "rdx", "rbx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static const char *xmm_names[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};

    if (reg_class == REG_CLASS_GPR && reg_index >= 0 && reg_index < 14) {
        return gpr_names[reg_index];
    } else if (reg_class == REG_CLASS_XMM && reg_index >= 0 && reg_index < 8) {
        return xmm_names[reg_index];
    }
    return "spilled";
}

#endif /* ZERO_CC_PARSER_LIVE_RANGE_H */


