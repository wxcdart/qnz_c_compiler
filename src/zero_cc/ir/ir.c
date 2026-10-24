#include "zero_cc/types/types.h"
#define ZERO_CC_GLOBAL_VARS_IMPL
#include "zero_cc/types/global_vars.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/types/symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-local pointer to current compiler during IR emission
 * This allows per-thread IR emission without data races in multi-threaded compilation */
__thread ZeroCCCompiler *g_compiler = NULL;

/* Set the current compiler (call before IR emission) */
void ir_set_compiler(ZeroCCCompiler *c) {
    g_compiler = c;
}

/* Frame slot addressing (Option 4: -fomit-frame-pointer).
 * Frame-slot IR operands are recorded FP-relative. When the frame pointer
 * is omitted for a fixed-frame function, slots are addressed from SP with a
 * constant delta (LoweringState.frame_delta). These accessors keep every
 * emission site uniform. */
int zero_cc_frame_base_reg(const ZeroCCCompiler *c) {
    return c->lowering.omit_frame_pointer ? c->isa_backend->sp_reg : c->isa_backend->fp_reg;
}

int32_t zero_cc_frame_disp(const ZeroCCCompiler *c, int32_t off) {
    return c->lowering.omit_frame_pointer ? (off + c->lowering.frame_delta) : off;
}

/* Get the current compiler's IR arena */
static inline IRArena *ir_arena(void) {
    return g_compiler ? &g_compiler->ir_arena : NULL;
}

/* Get the current compiler's call args arena */
static inline IRCallArgsArena *ir_call_arena(void) {
    return g_compiler ? &g_compiler->ir_call_args : NULL;
}

/* Get instruction buffer (for live range analysis) */
const Instr *ir_get_buffer(void) {
    IRArena *arena = ir_arena();
    return arena ? arena->buffer : NULL;
}

/* Reset IR buffer for next function - resets arena count (reuses memory) */
void ir_reset(void) {
    IRArena *arena = ir_arena();
    if (arena && arena->buffer) {
        arena->count = 0;
    }

    IRCallArgsArena *call_arena = ir_call_arena();
    if (call_arena && call_arena->table) {
        call_arena->count = 0;
    }

    /* A new function's IR invalidates any cached CFG (Sprint 35 Phase 2). */
    if (g_compiler) {
        g_compiler->cfg_version++;
        g_compiler->cfg_cache.valid = false;
    }
}

/* Get instruction count */
size_t ir_get_count(void) {
    IRArena *arena = ir_arena();
    return arena ? arena->count : 0;
}

/* Access instruction by index */
Instr* ir_get(size_t idx) {
    IRArena *arena = ir_arena();
    if (!arena || !arena->buffer || idx >= arena->count) return NULL;
    return &arena->buffer[idx];
}

/* Insert instruction at specific index (shifts existing instructions) */
void ir_insert_at(size_t idx, Instr instr) {
    IRArena *arena = ir_arena();
    if (!arena || !arena->buffer) {
        fprintf(stderr, "Error: IR arena not initialized for insertion\n");
        exit(1);
    }

    if (idx > arena->count) {
        fprintf(stderr, "Error: IR insert index %zu out of bounds (count=%zu)\n", idx, arena->count);
        exit(1);
    }

    Instr *ptr = ir_arena_insert(arena, idx, instr);
    if (!ptr) {
        fprintf(stderr, "Error: IR arena insertion failed\n");
        exit(1);
    }
}

/* Record call arguments for an instruction */
void ir_call_args_record(size_t instr_index, int count, const int *temps, const int *is_fp) {
    IRCallArgsArena *call_arena = ir_call_arena();
    if (!call_arena || !call_arena->table) return;

    /* Dynamic growth handles capacity automatically */
    ir_call_args_arena_record(call_arena, instr_index, count, temps, is_fp);
}

/* Find call arguments for an instruction */
const IRCallArgs *ir_call_args_find(size_t instr_index) {
    IRCallArgsArena *call_arena = ir_call_arena();
    if (!call_arena || !call_arena->table) return NULL;

    for (size_t i = 0; i < call_arena->count; i++) {
        if (call_arena->table[i].instr_index == instr_index) {
            return &call_arena->table[i];
        }
    }
    return NULL;
}

/* IR Emitter Functions - Arena-based with global API */
void ir_emit(Instr instr) {
    IRArena *arena = ir_arena();
    if (!arena || !arena->buffer) {
        fprintf(stderr, "Error: IR arena not initialized\n");
        exit(1);
    }

    /* restrict alias-group tagging: loads/stores inherit the group of their
     * address temp; pointer arithmetic propagates the group to its result. */
    if (g_compiler) {
        /* C2b: the alias-group table is 256 entries - higher temp ids simply
         * stay unconstrained (group 0), matching the guard in ast_lowering_expr.c. */
        uint8_t *tg = g_compiler->lowering.temp_alias_group;
        switch (instr.op) {
            case OP_DEREF: case OP_LOAD_BYTE: case OP_LOAD_16: case OP_LOAD_32:
                if (instr.temp_src1 != TEMP_NONE && instr.temp_src1 < 256) instr.alias_group = tg[instr.temp_src1];
                break;
            case OP_STORE_BYTE: case OP_STORE_16: case OP_STORE_32: case OP_STORE_64:
                if (instr.temp_src2 != TEMP_NONE && instr.temp_src2 < 256) instr.alias_group = tg[instr.temp_src2];
                break;
            case OP_MOV_REG: case OP_ADD_IMM: case OP_SUB_IMM:
                if (instr.temp_dest != TEMP_NONE && instr.temp_src1 != TEMP_NONE &&
                    instr.temp_dest < 256 && instr.temp_src1 < 256)
                    tg[instr.temp_dest] = tg[instr.temp_src1];
                break;
            case OP_ADD: case OP_SUB:
                if (instr.temp_dest != TEMP_NONE && instr.temp_src1 != TEMP_NONE && instr.temp_src2 != TEMP_NONE &&
                    instr.temp_dest < 256 && instr.temp_src1 < 256 && instr.temp_src2 < 256) {
                    uint8_t g1 = tg[instr.temp_src1], g2 = tg[instr.temp_src2];
                    tg[instr.temp_dest] = (g1 && g2 && g1 != g2) ? 0 : (g1 ? g1 : g2);
                }
                break;
            default:
                break;
        }
    }

    Instr *ptr = ir_arena_emit(arena, instr);
    if (!ptr) {
        fprintf(stderr, "Error: IR arena allocation failed\n");
        exit(1);
    }
}

void ir_emit_label(uint16_t label_idx) {
    Instr instr = {.op = OP_LABEL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_je(uint16_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JE, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jcond(uint16_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JCOND, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = REG_FLAGS, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jne(uint16_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JNE, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

void ir_emit_jmp_label(uint16_t label_idx) {
    Instr instr = {.op = OP_JMP_LABEL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(instr);
}

/* Clear IR buffer for next function - resets arena count (reuses memory) */
void ir_clear(void) {
    IRArena *arena = ir_arena();
    if (arena && arena->buffer) {
        arena->count = 0;
    }

    IRCallArgsArena *call_arena = ir_call_arena();
    if (call_arena && call_arena->table) {
        call_arena->count = 0;
    }
}

/* Debug: Dump IR to file */
void ir_dump(FILE *f) {
    IRArena *arena = ir_arena();
    if (!arena || !arena->buffer) return;

    size_t count = arena->count;
    for (size_t i = 0; i < count; i++) {
        Instr *instr = &arena->buffer[i];

        /* Print instruction index */
        fprintf(f, "%4zu: ", i);

        /* Print instruction */
        switch (instr->op) {
            case OP_MOV:
                         if (instr->temp_src1== TEMP_NONE) {
                             fprintf(f, "LOAD r%u, [fp%+lld]", instr->temp_dest, (long long)instr->imm64);
                         } else {
                             fprintf(f, "MOV r%u, ", instr->temp_dest);
                             if (instr->flags_in == REG_FLAGS) fprintf(f, "[flags] ");
                             fprintf(f, "r%u", instr->temp_src1);
                         }
                         break;
            case OP_MOV_IMM: fprintf(f, "MOV_IMM r%u, %lld", instr->temp_dest, (long long)instr->imm64);
                             break;
            case OP_MOV_MEM: fprintf(f, "MOV_MEM [fp%+lld], r%u", (long long)instr->imm64, instr->temp_src1);
                             break;
            case OP_ADD: fprintf(f, "ADD r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SUB: fprintf(f, "SUB r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_MUL: fprintf(f, "MUL r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_DIV: fprintf(f, "DIV r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_MOD: fprintf(f, "MOD r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_AND: fprintf(f, "AND r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_OR:  fprintf(f, "OR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_XOR: fprintf(f, "XOR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SHL: fprintf(f, "SHL r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SHR: fprintf(f, "SHR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_SAR: fprintf(f, "SAR r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_NEG: fprintf(f, "NEG r%u, r%u", instr->temp_dest, instr->temp_src1);
                         break;
            case OP_SEXT8: case OP_SEXT16: case OP_SEXT32:
            case OP_ZEXT8: case OP_ZEXT16: case OP_ZEXT32:
                fprintf(f, "%s r%u", ir_opcode_name(instr->op), instr->temp_src1);
                break;
            case OP_NOT: fprintf(f, "NOT r%u, r%u", instr->temp_dest, instr->temp_src1);
                         break;
            case OP_CMP: fprintf(f, "CMP r%u, ", instr->temp_src1);
                         if (instr->flags_in == REG_FLAGS) fprintf(f, "[flags] ");
                         fprintf(f, "r%u", instr->temp_src2);
                         break;
            case OP_COND_SET: fprintf(f, "COND_SET r%u, cc_%u", instr->temp_dest, instr->cond);
                              break;
            case OP_JCOND: fprintf(f, "JCOND cc_%u, L%u", instr->cond, instr->label_idx);
                           break;
            case OP_JMP: fprintf(f, "JMP L%u", instr->label_idx);
                         break;
            case OP_JE: fprintf(f, "JE r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_JNE: fprintf(f, "JNE r%u, L%u", instr->temp_src1, instr->label_idx);
                         break;
            case OP_JL: fprintf(f, "JL r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_JLE: fprintf(f, "JLE r%u, L%u", instr->temp_src1, instr->label_idx);
                         break;
            case OP_JG: fprintf(f, "JG r%u, L%u", instr->temp_src1, instr->label_idx);
                        break;
            case OP_RET: fprintf(f, "RET");
                         break;
            case OP_CALL: fprintf(f, "CALL fn_%u", (unsigned)instr->imm);
                          break;
            case OP_LEA: fprintf(f, "LEA r%u, [fp%+lld]", instr->temp_dest, (long long)instr->imm64);
                         break;
            case OP_FLOAD: fprintf(f, "FLOAD xmm%u, [fp%+lld]", instr->temp_dest, (long long)instr->imm64);
                           break;
            case OP_FSTORE: fprintf(f, "FSTORE [fp%+lld], xmm%u", (long long)instr->imm64, instr->temp_src1);
                            break;
            case OP_FADD: fprintf(f, "FADD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FSUB: fprintf(f, "FSUB xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FMUL: fprintf(f, "FMUL xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_FDIV: fprintf(f, "FDIV xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_CVTSI2SD: fprintf(f, "CVTSI2SD xmm%u, r%u", instr->temp_dest, instr->temp_src1);
                              break;
            case OP_CVTTSD2SI: fprintf(f, "CVTTSD2SI r%u, xmm%u", instr->temp_dest, instr->temp_src1);
                               break;
            /* SIMD operations */
            case OP_VADDSS: fprintf(f, "VADDSS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VSUBSS: fprintf(f, "VSUBSS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VMULSS: fprintf(f, "VMULSS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VDIVSS: fprintf(f, "VDIVSS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VADDSD: fprintf(f, "VADDSD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VSUBSD: fprintf(f, "VSUBSD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VMULSD: fprintf(f, "VMULSD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VDIVSD: fprintf(f, "VDIVSD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VMOV: fprintf(f, "VMOV xmm%u, xmm%u", instr->temp_dest, instr->temp_src1);
                          break;
            case OP_VMOVSS: fprintf(f, "VMOVSS xmm%u, xmm%u", instr->temp_dest, instr->temp_src1);
                            break;
            case OP_VMOVSD: fprintf(f, "VMOVSD xmm%u, xmm%u", instr->temp_dest, instr->temp_src1);
                            break;
            case OP_VLOAD: fprintf(f, "VLOAD xmm%u, [r%u]", instr->temp_dest, instr->temp_src1);
                           break;
            case OP_VSTORE: fprintf(f, "VSTORE [r%u], xmm%u", instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VLOADSS: fprintf(f, "VLOADSS xmm%u, [r%u]", instr->temp_dest, instr->temp_src1);
                             break;
            case OP_VLOADSD: fprintf(f, "VLOADSD xmm%u, [r%u]", instr->temp_dest, instr->temp_src1);
                             break;
            case OP_VSTORESS: fprintf(f, "VSTORESS [r%u], xmm%u", instr->temp_src1, instr->temp_src2);
                              break;
            case OP_VSTORESD: fprintf(f, "VSTORESD [r%u], xmm%u", instr->temp_src1, instr->temp_src2);
                              break;
            case OP_VCVT_SI2SS: fprintf(f, "VCVT_SI2SS xmm%u, r%u", instr->temp_dest, instr->temp_src1);
                                break;
            case OP_VCVT_SS2SI: fprintf(f, "VCVT_SS2SI r%u, xmm%u", instr->temp_dest, instr->temp_src1);
                                break;
            case OP_VCVT_SI2SD: fprintf(f, "VCVT_SI2SD xmm%u, r%u", instr->temp_dest, instr->temp_src1);
                                break;
            case OP_VCVT_SD2SI: fprintf(f, "VCVT_SD2SI r%u, xmm%u", instr->temp_dest, instr->temp_src1);
                                break;
            case OP_VCMPSS: fprintf(f, "VCMPSS xmm%u, xmm%u, cc=%u", instr->temp_src1, instr->temp_src2, instr->cond);
                            break;
            case OP_VCMPSD: fprintf(f, "VCMPSD xmm%u, xmm%u, cc=%u", instr->temp_src1, instr->temp_src2, instr->cond);
                            break;
            case OP_VAND: fprintf(f, "VAND xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_VOR: fprintf(f, "VOR xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                         break;
            case OP_VXOR: fprintf(f, "VXOR xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_VSHUF: fprintf(f, "VSHUF xmm%u, xmm%u, xmm%u, imm=%u", instr->temp_dest, instr->temp_src1, instr->temp_src2, instr->imm);
                           break;
            case OP_VANDN: fprintf(f, "VANDN xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                          break;
            case OP_VORPS: fprintf(f, "VORPS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                           break;
            case OP_VSUBPS: fprintf(f, "VSUBPS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VMULPS: fprintf(f, "VMULPS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VADDPS: fprintf(f, "VADDPS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VDIVPS: fprintf(f, "VDIVPS xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VADDPD: fprintf(f, "VADDPD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VPADDD: fprintf(f, "VPADDD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VPSUBD: fprintf(f, "VPSUBD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_VPMULLD: fprintf(f, "VPMULLD xmm%u, xmm%u, xmm%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                            break;
            case OP_LABEL: fprintf(f, "L%u:", instr->label_idx);
                           break;
            case OP_PROLOGUE: fprintf(f, "PROLOGUE %lld", (long long)instr->imm64);
                              break;
            case OP_EPILOGUE: fprintf(f, "EPILOGUE");
                              break;
            case OP_NOP: fprintf(f, "NOP");
                         break;
            case OP_DEAD: fprintf(f, "DEAD");
                          break;
            case OP_MOV_REG: fprintf(f, "MOV_REG r%u, r%u", instr->temp_dest, instr->temp_src1);
                             break;
            case OP_MOV_GLOBAL: fprintf(f, "MOV_GLOBAL r%u, %s", instr->temp_dest, (char*)instr->imm64);
                                break;
            case OP_VLA_ALLOC: fprintf(f, "VLA_ALLOC r%u, r%u", instr->temp_dest, instr->temp_src1);
                               break;
            case OP_CALLEE_SAVE_STORES: fprintf(f, "CALLEE_SAVE_STORES mask=0x%llx", (unsigned long long)instr->imm64);
                                       break;
            case OP_CALLEE_SAVE_LOADS: fprintf(f, "CALLEE_SAVE_LOADS mask=0x%llx", (unsigned long long)instr->imm64);
                                       break;
            case OP_PARAM_SAVE_INT: fprintf(f, "PARAM_SAVE_INT reg=%d offset=%lld", (int)instr->imm, (long long)instr->imm64);
                                    break;
            case OP_PARAM_SAVE_FP: fprintf(f, "PARAM_SAVE_FP reg=%d offset=%lld", (int)instr->imm, (long long)instr->imm64);
                                   break;
            case OP_PARAM_LOAD: fprintf(f, "PARAM_LOAD r%u, %s_arg=%d", instr->temp_dest, instr->cond ? "fp" : "int", (int)instr->imm);
                                break;
            case OP_VARIADIC_SPILL: fprintf(f, "VARIADIC_SPILL param_count=%d base_offset=%lld", (int)instr->imm, (long long)instr->imm64);
                                    break;
            case OP_ATOMIC_XADD: fprintf(f, "ATOMIC_XADD r%u, [r%u], r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                                 break;
            case OP_ATOMIC_XCHG: fprintf(f, "ATOMIC_XCHG r%u, [r%u], r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2);
                                 break;
            case OP_SYSCALL: fprintf(f, "SYSCALL args=%u", instr->temp_dest);
                             break;
            case OP_STORE_BYTE: fprintf(f, "STORE_BYTE [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                                break;
            case OP_STORE_16: fprintf(f, "STORE_16 [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                              break;
            case OP_LOAD_BYTE: fprintf(f, "LOAD_BYTE r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                               break;
            case OP_LOAD_16: fprintf(f, "LOAD_16 r%u, [fp%+d]", instr->temp_dest, (int)instr->imm);
                             break;
            case OP_CMP_ZERO: fprintf(f, "CMP_ZERO r%u", instr->temp_src1);
                              break;
            case OP_STORE_32: fprintf(f, "STORE_32 [r%u], r%u", instr->temp_src2, instr->temp_src1);
                              break;
            case OP_DEREF: fprintf(f, "DEREF r%u, r%u", instr->temp_dest, instr->temp_src1);
                           break;
            default: fprintf(f, "OP_%u", instr->op);
                     break;
        }

        /* Print flag annotations */
        fprintf(f, " [flags_out=");
        if (instr->flags_out == REG_FLAGS) fprintf(f, "r0");
        else fprintf(f, "r%u", instr->flags_out);
        fprintf(f, "] [flags_in=");
        if (instr->flags_in == REG_FLAGS) fprintf(f, "r0");
        else fprintf(f, "r%u", instr->flags_in);
        fprintf(f, "]\n");
    }
}
