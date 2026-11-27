#include "zero_cc/opt/opt_loop_delete.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/types/symbols.h"
#include "zero_cc/opt/cfg.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/vregs.h"

/* Helper: find instruction index of a label */
static int loop_delete_find_label_pos(ZeroCCCompiler *c, uint16_t label_idx) {
    int count = (int)ir_get_count(c);
    for (int i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (ins && ins->op == OP_LABEL && ins->label_idx == label_idx) {
            return i;
        }
    }
    return -1;
}

/* Check if an opcode has unremovable external side effects */
static bool loop_delete_has_external_side_effect(uint16_t op) {
    switch (op) {
        case OP_CALL: case OP_CALL_INDIRECT: case OP_SYSCALL: case OP_CLONE:
        case OP_ASM: case OP_VLA_ALLOC:
        case OP_PROLOGUE: case OP_EPILOGUE: case OP_RET:
        case OP_SETJMP: case OP_LONGJMP: case OP_NONLOCAL_GOTO:
        case OP_TRAP: case OP_OVF_TRAP: case OP_ASAN_CHECK: case OP_MFENCE:
        case OP_ATOMIC_XADD: case OP_ATOMIC_CMPXCHG: case OP_ATOMIC_CMPXCHG_BOOL:
        case OP_ATOMIC_XCHG: case OP_ATOMIC_OR: case OP_ATOMIC_AND: case OP_ATOMIC_XOR:
        case OP_STORE_BYTE: case OP_STORE_16: case OP_STORE_32: case OP_STORE_64:
        case OP_STORE128: case OP_FSTORE: case OP_VSTORE: case OP_V256_STORE:
        case OP_V512_STORE: case OP_V512_MASK_STORE: case OP_VSTORESS: case OP_VSTORESD:
        case OP_VSTORESS_RAW: case OP_KMOVW_TO_K1: case OP_SVE_ST1:
        case OP_RVV_VSE: case OP_RVV_VSSE: case OP_RVV_VSUXEI:
        case OP_V128_STORE_STRIDED: case OP_V128_SCATTER:
        case OP_PUSH: case OP_POP:
        case OP_LOAD_BYTE: case OP_LOAD_16: case OP_LOAD_32: case OP_LOAD64:
        case OP_LOAD128: case OP_DEREF: case OP_FLOAD:
            return true;
        default:
            return false;
    }
}

/* Check if a loop is single-entry and clean */
static bool loop_delete_is_clean_entry(const ZeroCCCFG *cfg, int h, int u) {
    for (int b = h; b <= u; b++) {
        for (int p = 0; p < cfg->blocks[b].pred_count; p++) {
            int pred = cfg->blocks[b].preds[p];
            if (pred >= h && pred <= u) continue;
            if (b != h) return false; /* side entry into loop body */
        }
    }
    return true;
}

/* Check whether any temporary defined inside [start, end) is read outside */
static bool loop_delete_check_escaping_temps(ZeroCCCompiler *c, int start, int end) {
    int count = (int)ir_get_count(c);
    for (int i = start; i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_NOP || ins->op == OP_DEAD) continue;
        uint32_t dest = ins->temp_dest;
        if (dest == TEMP_NONE || dest == 0) continue;

        for (int j = 0; j < count; j++) {
            if (j >= start && j < end) continue;
            Instr *other = ir_get(c, j);
            if (!other || other->op == OP_NOP || other->op == OP_DEAD) continue;
            if (other->temp_src1 == dest || other->temp_src2 == dest) {
                return false; /* value escapes loop */
            }
        }
    }
    return true;
}

/* Check whether any stack store inside [start, end) is read or address-taken outside */
static bool loop_delete_check_escaping_slots(ZeroCCCompiler *c, int start, int end) {
    int count = (int)ir_get_count(c);
    for (int i = start; i < end; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op != OP_MOV_MEM) continue;
        int64_t slot = ins->imm64;

        for (int j = 0; j < count; j++) {
            if (j >= start && j < end) continue;
            Instr *other = ir_get(c, j);
            if (!other) continue;
            if (other->op == OP_MOV && other->temp_src1 == TEMP_NONE && other->imm64 == slot) {
                return false; /* stack load outside loop reads this slot */
            }
            if (other->op == OP_LEA && other->imm64 == slot) {
                return false; /* address of slot taken outside loop */
            }
        }
    }
    return true;
}

/* Dead Loop Deletion pass implementation */
bool opt_loop_delete(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp || c->lowering.has_capture_mirror) return false;
    if (!cfg_build(c)) return false;

    ZeroCCCFG *cfg = &c->cfg_cache;
    if (cfg->loop_count == 0) return false;

    bool any_deleted = false;

    for (int l = 0; l < cfg->loop_count; l++) {
        int h = cfg->loops[l].header;
        int u = cfg->loops[l].backedge_from;
        if (h < 0 || u < 0 || h >= cfg->block_count || u >= cfg->block_count) continue;

        int start = (int)cfg->blocks[h].start;
        int end = (int)cfg->blocks[u].end;
        if (start >= end) continue;

        /* Verify single-entry clean loop */
        if (!loop_delete_is_clean_entry(cfg, h, u)) continue;

        /* Find the loop exit label from the header block */
        uint16_t exit_label = 0;
        bool found_exit = false;

        for (int i = (int)cfg->blocks[h].start; i < (int)cfg->blocks[h].end; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            switch (ins->op) {
                case OP_JE: case OP_JNE:
                case OP_JL: case OP_JLE:
                case OP_JG:
                case OP_JCOND: {
                    int pos = loop_delete_find_label_pos(c, ins->label_idx);
                    if (pos < start || pos >= end) {
                        exit_label = ins->label_idx;
                        found_exit = true;
                    }
                    break;
                }
                default:
                    break;
            }
            if (found_exit) break;
        }

        if (!found_exit) continue;

        /* Check for external side effects */
        bool side_effect = false;
        for (int i = start; i < end; i++) {
            Instr *ins = ir_get(c, i);
            if (!ins) continue;
            if (loop_delete_has_external_side_effect(ins->op)) {
                side_effect = true;
                break;
            }
        }
        if (side_effect) continue;

        /* Check for escaping stack slot stores */
        if (!loop_delete_check_escaping_slots(c, start, end)) continue;

        /* Check for escaping temporaries */
        if (!loop_delete_check_escaping_temps(c, start, end)) continue;

        /* The loop is pure, countable, and all computed values are dead!
         * Delete the loop: replace loop header entry with an unconditional
         * jump to exit_label, and NOP out the loop body and backedge. */
        int jmp_pos = start;
        if (ir_get(c, start)->op == OP_LABEL) {
            jmp_pos = start + 1;
        }

        if (jmp_pos < end) {
            Instr *jmp_ins = ir_get(c, jmp_pos);
            jmp_ins->op = OP_JMP;
            jmp_ins->temp_dest = TEMP_NONE;
            jmp_ins->temp_src1 = TEMP_NONE;
            jmp_ins->temp_src2 = TEMP_NONE;
            jmp_ins->cond = 0;
            jmp_ins->flags_out = 0xFF;
            jmp_ins->flags_in = 0xFF;
            jmp_ins->label_idx = exit_label;
            jmp_ins->imm = 0;
            jmp_ins->imm64 = 0;

            for (int k = jmp_pos + 1; k < end; k++) {
                Instr *nop_ins = ir_get(c, k);
                if (!nop_ins) continue;
                nop_ins->op = OP_NOP;
                nop_ins->temp_dest = TEMP_NONE;
                nop_ins->temp_src1 = TEMP_NONE;
                nop_ins->temp_src2 = TEMP_NONE;
                nop_ins->imm = 0;
                nop_ins->imm64 = 0;
            }
            any_deleted = true;
        }
    }

    if (any_deleted) {
        c->cfg_version++;
    }

    return any_deleted;
}
