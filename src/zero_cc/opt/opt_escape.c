#include "zero_cc/opt/opt_escape.h"
#include <stdlib.h>
#include <string.h>
#include "zero_cc/core/core.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/types/vregs.h"

#define MAX_FREE_CALLS 16
#define MAX_STACK_PROMOTION_SIZE 4096

/* Trace the size argument of a malloc call backward to a constant */
static int64_t escape_find_constant_size(ZeroCCCompiler *c, int call_idx, uint32_t sz_temp) {
    int cur = call_idx - 1;
    while (cur >= 0) {
        Instr *si = ir_get(c, cur);
        if (!si) { cur--; continue; }
        if (si->temp_dest == sz_temp) {
            if (si->op == OP_MOV_IMM) {
                return si->imm64 ? si->imm64 : (int64_t)si->imm;
            }
            if (si->op == OP_MUL) {
                int64_t v1 = escape_find_constant_size(c, cur, si->temp_src1);
                int64_t v2 = escape_find_constant_size(c, cur, si->temp_src2);
                if (v1 > 0 && v2 > 0) return v1 * v2;
                return -1;
            }
            if (si->op == OP_SHL) {
                int64_t v1 = escape_find_constant_size(c, cur, si->temp_src1);
                int64_t v2 = escape_find_constant_size(c, cur, si->temp_src2);
                if (v1 > 0 && v2 >= 0 && v2 < 64) return v1 << v2;
                return -1;
            }
            if (si->op == OP_MOV_REG || si->op == OP_SEXT32 || si->op == OP_ZEXT32) {
                sz_temp = si->temp_src1;
            } else if (si->op == OP_MOV && si->temp_src1 == TEMP_NONE) {
                int64_t slot = si->imm64 ? si->imm64 : (int64_t)si->imm;
                for (int k = cur - 1; k >= 0; k--) {
                    Instr *ki = ir_get(c, k);
                    if (ki && ki->op == OP_MOV_MEM && (ki->imm64 == slot || ki->imm == (int16_t)slot)) {
                        uint32_t st_temp = ki->temp_src1;
                        for (int m = k - 1; m >= 0; m--) {
                            Instr *mi = ir_get(c, m);
                            if (mi && mi->temp_dest == st_temp && mi->op == OP_MOV_IMM) {
                                return mi->imm64 ? mi->imm64 : (int64_t)mi->imm;
                            }
                        }
                        break;
                    }
                }
                break;
            } else {
                break;
            }
        }
        cur--;
    }
    return -1;
}

/* Check if a temporary used at use_idx traces back to the malloc allocation */
static bool escape_traces_to_malloc(ZeroCCCompiler *c, int use_idx, uint32_t temp,
                                    int ptr_dest_temp, int64_t p_slot) {
    if (temp == TEMP_NONE) return false;
    if ((int)temp == ptr_dest_temp) return true;

    for (int i = use_idx - 1; i >= 0; i--) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (ins->temp_dest == temp) {
            if (ins->op == OP_MOV_REG) {
                return escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot);
            }
            if (ins->op == OP_MOV && ins->temp_src1 == TEMP_NONE) {
                int64_t slot = ins->imm64 ? ins->imm64 : (int64_t)ins->imm;
                if (p_slot != 0x7FFFFFFF && slot == p_slot) {
                    return true;
                }
                return false;
            }
            if (ins->op == OP_ADD) {
                return escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot) ||
                       escape_traces_to_malloc(c, i, ins->temp_src2, ptr_dest_temp, p_slot);
            }
            return false;
        }
    }
    return false;
}

/* Try to promote a single malloc call to a stack allocation */
static bool escape_try_promote_malloc(ZeroCCCompiler *c, int call_idx) {
    Instr *call_ins = ir_get(c, call_idx);
    if (!call_ins || call_ins->op != OP_CALL) return false;

    const char *fn_name = (const char *)(intptr_t)call_ins->imm64;
    if (!fn_name || strcmp(fn_name, "malloc") != 0) return false;

    int64_t alloc_size = escape_find_constant_size(c, call_idx, call_ins->temp_src1);
    if (alloc_size <= 0 || alloc_size > MAX_STACK_PROMOTION_SIZE) {
        return false;
    }

    int count = (int)ir_get_count(c);
    int ptr_dest_temp = -1;
    int mov_dest_idx = -1;

    if (call_idx + 1 < count) {
        Instr *next = ir_get(c, call_idx + 1);
        if (next && next->op == OP_MOV_REG && (int)next->temp_src1 == VREG_0) {
            ptr_dest_temp = (int)next->temp_dest;
            mov_dest_idx = call_idx + 1;
        }
    }

    /* Dead allocation: malloc called but return value never captured */
    if (ptr_dest_temp < 0) {
        call_ins->op = OP_NOP;
        call_ins->temp_dest = TEMP_NONE;
        call_ins->temp_src1 = TEMP_NONE;
        call_ins->temp_src2 = TEMP_NONE;
        return true;
    }

    int64_t p_slot = 0x7FFFFFFF;
    if (mov_dest_idx >= 0 && mov_dest_idx + 1 < count) {
        Instr *st = ir_get(c, mov_dest_idx + 1);
        if (st && st->op == OP_MOV_MEM && (int)st->temp_src1 == ptr_dest_temp) {
            p_slot = st->imm64 ? st->imm64 : (int64_t)st->imm;
        }
    }

    bool escapes = false;
    int free_calls[MAX_FREE_CALLS];
    int free_call_count = 0;

    for (int i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op == OP_DEAD || ins->op == OP_NOP) continue;
        if (i == call_idx || i == mov_dest_idx) continue;

        /* Check function calls */
        if (ins->op == OP_CALL) {
            const char *fn = (const char *)(intptr_t)ins->imm64;
            if (fn && strcmp(fn, "free") == 0) {
                if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot)) {
                    if (free_call_count < MAX_FREE_CALLS) {
                        free_calls[free_call_count++] = i;
                    }
                    continue;
                }
            }
            /* Any other call that receives the pointer escapes */
            if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot) ||
                escape_traces_to_malloc(c, i, ins->temp_src2, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
            /* Check OP_CALL_ARG */
            int call_arg_count = (int)ins->temp_dest;
            for (int a = 2; a < call_arg_count; a++) {
                int ca_idx = i + 1 + (a - 2);
                if (ca_idx >= count) break;
                Instr *ca = ir_get(c, ca_idx);
                if (ca && ca->op == OP_CALL_ARG &&
                    escape_traces_to_malloc(c, ca_idx, ca->temp_src1, ptr_dest_temp, p_slot)) {
                    escapes = true;
                    break;
                }
            }
            if (escapes) break;
            continue;
        }

        if (ins->op == OP_CALL_INDIRECT || ins->op == OP_SYSCALL ||
            ins->op == OP_ASM || ins->op == OP_SETJMP) {
            if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot) ||
                escape_traces_to_malloc(c, i, ins->temp_src2, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
        }

        if (ins->op == OP_RET) {
            if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
        }

        /* Setting return register VREG_0 */
        if (ins->op == OP_MOV_REG && (int)ins->temp_dest == VREG_0) {
            if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
        }

        /* Global store */
        if (ins->op == OP_MOV_GLOBAL) {
            if (escape_traces_to_malloc(c, i, ins->temp_src1, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
        }

        /* Memory store */
        if (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
            ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
            ins->op == OP_STORE64 || ins->op == OP_FSTORE ||
            ins->op == OP_VSTORESS || ins->op == OP_VSTORESD) {
            bool is_int_store = (ins->op == OP_STORE_BYTE || ins->op == OP_STORE_16 ||
                                 ins->op == OP_STORE_32 || ins->op == OP_STORE_64 ||
                                 ins->op == OP_STORE64);
            uint32_t val_s = is_int_store ? ins->temp_src1 : ins->temp_src2;

            if (escape_traces_to_malloc(c, i, val_s, ptr_dest_temp, p_slot)) {
                escapes = true;
                break;
            }
            continue;
        }

        /* Memory load: accessing allocated memory is safe and non-escaping */
        if (ins->op == OP_LOAD_BYTE || ins->op == OP_LOAD_16 ||
            ins->op == OP_LOAD_32 || ins->op == OP_LOAD64 ||
            ins->op == OP_FLOAD || ins->op == OP_VLOADSS || ins->op == OP_VLOADSD) {
            continue;
        }
    }

    if (escapes) {
        return false;
    }

    /* Promote heap allocation to local stack frame */
    int32_t aligned_size = (int32_t)((alloc_size + 15) & ~15);
    c->current_stack_frame = (c->current_stack_frame + 15) & ~15;
    c->current_stack_frame += aligned_size;
    int32_t slot = c->current_stack_frame;

    /* Replace malloc call with OP_NOP */
    call_ins->op = OP_NOP;
    call_ins->temp_dest = TEMP_NONE;
    call_ins->temp_src1 = TEMP_NONE;
    call_ins->temp_src2 = TEMP_NONE;

    /* Replace MOV_REG r_dest, VREG_0 with OP_LEA r_dest, [rbp - slot] */
    if (mov_dest_idx >= 0) {
        Instr *mov_ins = ir_get(c, mov_dest_idx);
        mov_ins->op = OP_LEA;
        mov_ins->temp_dest = ptr_dest_temp;
        mov_ins->temp_src1 = TEMP_NONE;
        mov_ins->temp_src2 = TEMP_NONE;
        mov_ins->imm = (int16_t)(-slot);
        mov_ins->imm64 = -slot;
        zc_temp_tag_set(c, ptr_dest_temp, ZC_TAG_PTR);
    }

    /* Replace all matching free(p) calls with OP_NOP */
    for (int f = 0; f < free_call_count; f++) {
        int f_idx = free_calls[f];
        Instr *fi = ir_get(c, f_idx);
        fi->op = OP_NOP;
        fi->temp_dest = TEMP_NONE;
        fi->temp_src1 = TEMP_NONE;
        fi->temp_src2 = TEMP_NONE;
        if (f_idx > 0) {
            Instr *prev = ir_get(c, f_idx - 1);
            if (prev && (prev->op == OP_MOV_REG || prev->op == OP_MOV)) {
                prev->op = OP_NOP;
                prev->temp_dest = TEMP_NONE;
                prev->temp_src1 = TEMP_NONE;
            }
        }
    }

    return true;
}

bool opt_escape(ZeroCCCompiler *c) {
    if (!c || c->lowering.has_setjmp) return false;

    int count = (int)ir_get_count(c);
    bool any_changed = false;

    for (int i = 0; i < count; i++) {
        Instr *ins = ir_get(c, i);
        if (!ins || ins->op != OP_CALL) continue;

        const char *fn_name = (const char *)(intptr_t)ins->imm64;
        if (fn_name && strcmp(fn_name, "malloc") == 0) {
            if (escape_try_promote_malloc(c, i)) {
                any_changed = true;
            }
        }
    }

    return any_changed;
}
