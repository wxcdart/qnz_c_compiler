#include "zero_cc/types/types.h"
#include "zero_cc/types/global_vars.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/types/symbols.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Thread-reentrant compiler state: no g_compiler */

/* Frame slot addressing (Option 4: -fomit-frame-pointer).
 * Frame-slot IR operands are recorded FP-relative. When the frame pointer
 * is omitted for a fixed-frame function, slots are addressed from SP with a
 * constant delta (LoweringState.frame_delta). These accessors keep every
 * emission site uniform. */
int zero_cc_frame_base_reg(const ZeroCCCompiler *c) {
    return c->lowering.omit_frame_pointer ? c->isa_backend->sp_reg : c->isa_backend->fp_reg;
}

int32_t zero_cc_frame_disp_at(const ZeroCCCompiler *c, int32_t off, int32_t extra_sp) {
    return c->lowering.omit_frame_pointer ? (off + c->lowering.frame_delta + extra_sp) : off;
}

int32_t zero_cc_frame_disp(const ZeroCCCompiler *c, int32_t off) {
    return zero_cc_frame_disp_at(c, off, 0);
}



/* Get instruction buffer (for live range analysis) */
const Instr *ir_get_buffer(const ZeroCCCompiler *c) {
    return c ? c->ir_arena.buffer : NULL;
}

/* Sprint 118: per-temp type-tag table (lazy 64 KiB; indexed by temp id). */
uint8_t zc_temp_tag(const ZeroCCCompiler *c, uint32_t id) {
    if (!c || !c->temp_type_tags || id >= 65536) return 0;
    return c->temp_type_tags[id];
}

void zc_temp_tag_set(ZeroCCCompiler *c, uint32_t id, uint8_t tag) {
    if (!c || id >= 65536) return;
    if (!c->temp_type_tags) {
        c->temp_type_tags = (uint8_t *)calloc(1, 65536);
        if (!c->temp_type_tags) return;  /* table is advisory; never fatal */
        c->temp_type_tags_hi = 0;
    }
    c->temp_type_tags[id] = tag;
    if (id + 1 > c->temp_type_tags_hi) c->temp_type_tags_hi = id + 1;
}

void zc_temp_tags_reset(ZeroCCCompiler *c) {
    if (!c || !c->temp_type_tags) return;
    uint32_t hi = c->temp_type_tags_hi < 65536 ? c->temp_type_tags_hi : 65536;
    memset(c->temp_type_tags, 0, hi);
    c->temp_type_tags_hi = 0;
}

/* Allocate the next function-local temp id, skipping the TEMP_NONE (0xFFFF)
 * sentinel so a churned 32-bit id space (Sprint 151) can never collide with
 * the "no temp" marker. */
int zc_alloc_temp(ZeroCCCompiler *c) {
    int t = c->next_temp_id++;
    if (t == TEMP_NONE) t = c->next_temp_id++;
    return t;
}

/* Reset IR buffer for next function - resets arena count (reuses memory) */
void ir_reset(ZeroCCCompiler *c) {
    if (!c) return;
    if (c->ir_arena.buffer) {
        c->ir_arena.count = 0;
    }

    /* Sprint 118: temp ids restart at VREG_USER_START; clear the tag table. */
    zc_temp_tags_reset(c);

    /* A new function's IR invalidates any cached CFG (Sprint 35 Phase 2). */
    c->cfg_version++;
    c->cfg_cache.valid = false;
}

/* Get instruction count */
size_t ir_get_count(const ZeroCCCompiler *c) {
    return c ? c->ir_arena.count : 0;
}

/* Access instruction by index */
Instr* ir_get(ZeroCCCompiler *c, size_t idx) {
    if (!c || !c->ir_arena.buffer || idx >= c->ir_arena.count) return NULL;
    return &c->ir_arena.buffer[idx];
}

/* Insert instruction at specific index (shifts existing instructions) */
void ir_insert_at(ZeroCCCompiler *c, size_t idx, Instr instr) {
    if (!c || !c->ir_arena.buffer) {
        fprintf(stderr, "Error: IR arena not initialized for insertion\n");
        exit(1);
    }

    if (idx > c->ir_arena.count) {
        fprintf(stderr, "Error: IR insert index %zu out of bounds (count=%zu)\n", idx, c->ir_arena.count);
        exit(1);
    }

    Instr *ptr = ir_arena_insert(&c->ir_arena, idx, instr);
    if (!ptr) {
        fprintf(stderr, "Error: IR arena insertion failed\n");
        exit(1);
    }
}

/* Sprint 154: collect the payload of the OP_ASM at instr_index into `out`
 * (a stack IRAsmArgs) from the anchor + its OP_ASM_ARG/OP_ASM_LABEL
 * pseudo-run.  Returns `out` on success, NULL if not an OP_ASM anchor.
 * Replaces the former ir_asm_args_find side-table lookup. */
const IRAsmArgs *ir_asm_args_collect(const ZeroCCCompiler *c,
                                     size_t instr_index,
                                     IRAsmArgs *out) {
    if (!c || !out || !c->ir_arena.buffer ||
        instr_index >= c->ir_arena.count) return NULL;
    const Instr *anchor = &c->ir_arena.buffer[instr_index];
    if (anchor->op != OP_ASM) return NULL;

    memset(out, 0, sizeof(*out));
    out->template_str = (const char *)(uintptr_t)anchor->imm64;
    out->op_count = (int)anchor->temp_dest;
    out->clobber_mask = (uint32_t)(uint16_t)anchor->imm;
    out->has_memory = anchor->cond & 1;
    out->is_goto = (anchor->cond >> 1) & 1;
    out->goto_label_count = (int)anchor->width;
    if (out->op_count > IR_ASM_MAX_OPERANDS) out->op_count = IR_ASM_MAX_OPERANDS;
    if (out->goto_label_count > IR_ASM_MAX_LABELS) out->goto_label_count = IR_ASM_MAX_LABELS;

    size_t k = instr_index + 1;
    for (int o = 0; o < out->op_count; o++) {
        if (k >= c->ir_arena.count) break;
        const Instr *arg = &c->ir_arena.buffer[k++];
        if (arg->op != OP_ASM_ARG) break;
        out->ops[o].temp = (int32_t)arg->temp_src1;
        out->ops[o].kind = arg->cond;
        out->ops[o].width = arg->width;
        out->ops[o].digit = arg->imm;
        out->ops[o].fixed_reg = (arg->label_idx != 0)
            ? (int16_t)((int)arg->label_idx - 1) : -1;
    }
    for (int g = 0; g < out->goto_label_count; g++) {
        if (k >= c->ir_arena.count) break;
        const Instr *lg = &c->ir_arena.buffer[k++];
        if (lg->op != OP_ASM_LABEL) break;
        out->goto_label_names[g] = (const char *)(uintptr_t)lg->imm64;
        out->goto_labels[g] = lg->label_idx;
    }
    return out;
}

/* IR Emitter Functions - Arena-based with compiler context */
void ir_emit(ZeroCCCompiler *c, Instr instr) {
    if (!c || !c->ir_arena.buffer) {
        fprintf(stderr, "Error: IR arena not initialized\n");
        exit(1);
    }

    /* restrict alias-group tagging: loads/stores inherit the group of their
     * address temp; pointer arithmetic propagates the group to its result. */
    uint8_t *tg = c->lowering.temp_alias_group;
    switch (instr.op) {
        case OP_DEREF: case OP_LOAD_BYTE: case OP_LOAD_16: case OP_LOAD_32:
            if (zc_temp_is_user(instr.temp_src1) && instr.temp_src1 < 256) instr.alias_group = tg[instr.temp_src1];
            break;
        case OP_STORE_BYTE: case OP_STORE_16: case OP_STORE_32: case OP_STORE_64:
            if (zc_temp_is_user(instr.temp_src2) && instr.temp_src2 < 256) instr.alias_group = tg[instr.temp_src2];
            break;
        case OP_MOV_REG: case OP_ADD_IMM: case OP_SUB_IMM:
            if (zc_temp_is_user(instr.temp_dest) && zc_temp_is_user(instr.temp_src1) &&
                instr.temp_dest < 256 && instr.temp_src1 < 256)
                tg[instr.temp_dest] = tg[instr.temp_src1];
            break;
        case OP_ADD: case OP_SUB:
            if (zc_temp_is_user(instr.temp_dest) && zc_temp_is_user(instr.temp_src1) && zc_temp_is_user(instr.temp_src2) &&
                instr.temp_dest < 256 && instr.temp_src1 < 256 && instr.temp_src2 < 256) {
                uint8_t g1 = tg[instr.temp_src1], g2 = tg[instr.temp_src2];
                tg[instr.temp_dest] = (g1 && g2 && g1 != g2) ? 0 : (g1 ? g1 : g2);
            }
            break;
        default:
            break;
    }

    Instr *ptr = ir_arena_emit(&c->ir_arena, instr);
    if (!ptr) {
        fprintf(stderr, "Error: IR arena allocation failed\n");
        exit(1);
    }
}

void ir_emit_label(ZeroCCCompiler *c, uint16_t label_idx) {
    Instr instr = {.op = OP_LABEL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

void ir_emit_jmp(ZeroCCCompiler *c, uint16_t label_idx) {
    Instr instr = {.op = OP_JMP, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

void ir_emit_je(ZeroCCCompiler *c, uint32_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JE, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

void ir_emit_jcond(ZeroCCCompiler *c, uint32_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JCOND, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = REG_FLAGS, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

void ir_emit_jne(ZeroCCCompiler *c, uint32_t temp_cond, uint16_t label_idx) {
    Instr instr = {.op = OP_JNE, .temp_dest= TEMP_NONE, .temp_src1 = temp_cond, .temp_src2= TEMP_NONE, .cond = temp_cond, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

void ir_emit_jmp_label(ZeroCCCompiler *c, uint16_t label_idx) {
    Instr instr = {.op = OP_JMP_LABEL, .temp_dest= TEMP_NONE, .temp_src1= TEMP_NONE, .temp_src2= TEMP_NONE, .cond = 0, .flags_out = 0xFF, .flags_in = 0xFF, .imm = 0, .imm64 = 0, .label_idx = label_idx};
    ir_emit(c, instr);
}

/* Clear IR buffer for next function - resets arena count (reuses memory) */
void ir_clear(ZeroCCCompiler *c) {
    if (!c) return;
    if (c->ir_arena.buffer) {
        c->ir_arena.count = 0;
    }
}

/* Debug: Dump IR to file */
void ir_dump(const ZeroCCCompiler *c, FILE *f) {
    if (!c || !c->ir_arena.buffer) return;

    size_t count = c->ir_arena.count;
    for (size_t i = 0; i < count; i++) {
        Instr *instr = &c->ir_arena.buffer[i];

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
            case OP_OVF_TRAP: fprintf(f, "OVF_TRAP r%u", instr->temp_dest);
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
            case OP_ADD128: fprintf(f, "ADD128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_SUB128: fprintf(f, "SUB128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_MUL128: fprintf(f, "MUL128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_AND128: fprintf(f, "AND128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_OR128:  fprintf(f, "OR128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_XOR128: fprintf(f, "XOR128 r%u, r%u, r%u", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
            case OP_NOT128: fprintf(f, "NOT128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_NEG128: fprintf(f, "NEG128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_MOV128: fprintf(f, "MOV128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_MOV128_IMM: fprintf(f, "MOV128_IMM r%u, %lld", instr->temp_dest, (long long)instr->imm64); break;
            case OP_LOAD128: fprintf(f, "LOAD128 r%u, [r%u]", instr->temp_dest, instr->temp_src1); break;
            case OP_STORE128: fprintf(f, "STORE128 [r%u], r%u", instr->temp_src2, instr->temp_src1); break;
            case OP_ZEXT64_128: fprintf(f, "ZEXT64_128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_SEXT64_128: fprintf(f, "SEXT64_128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_TRUNC128_64: fprintf(f, "TRUNC128_64 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_LO128: fprintf(f, "LO128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_HI128: fprintf(f, "HI128 r%u, r%u", instr->temp_dest, instr->temp_src1); break;
            case OP_MK128: fprintf(f, "MK128 r%u, r%u(lo), r%u(hi)", instr->temp_dest, instr->temp_src1, instr->temp_src2); break;
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
            case OP_CVTSS2SD: fprintf(f, "CVTSS2SD xmm%u, xmm%u", instr->temp_dest, instr->temp_src1);
                                break;
            case OP_CVTSD2SS: fprintf(f, "CVTSD2SS xmm%u, xmm%u", instr->temp_dest, instr->temp_src1);
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
            case OP_JMP_INDIRECT: fprintf(f, "JMP_INDIRECT r%u", instr->temp_src1);
                                  break;
            case OP_LEA_LABEL: fprintf(f, "LEA_LABEL r%u, L%u", instr->temp_dest, instr->label_idx);
                               break;
            case OP_SETJMP: fprintf(f, "SETJMP [r%u], L%u", instr->temp_src1, instr->label_idx);
                            break;
            case OP_LONGJMP: fprintf(f, "LONGJMP [r%u], r%u", instr->temp_src1, instr->temp_src2);
                             break;
            case OP_NONLOCAL_GOTO: fprintf(f, "NONLOCAL_GOTO r%u, r%u -> fn%d:L%u",
                              instr->temp_src1, instr->temp_src2,
                              (int)instr->imm64, instr->label_idx);
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
            case OP_CLONE: fprintf(f, "CLONE args=%u", instr->temp_dest);
                           break;
            case OP_CALL_ARG: fprintf(f, "CALL_ARG r%u (is_fp=%u, stack_pos=%u)", instr->temp_src1, instr->cond, instr->width);
                              break;
            case OP_STORE_BYTE: fprintf(f, "STORE_BYTE [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                                break;
            case OP_STORE_16: fprintf(f, "STORE_16 [fp%+d], r%u", (int)instr->imm, instr->temp_src1);
                              break;
            case OP_LOAD_BYTE: fprintf(f, "LOAD_BYTE r%u, [r%d+fp%+d]", instr->temp_dest, (int)instr->temp_src1, (int)instr->imm);
                               break;
            case OP_LOAD_16: fprintf(f, "LOAD_16 r%u, [r%d+fp%+d]", instr->temp_dest, (int)instr->temp_src1, (int)instr->imm);
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
        fprintf(f, "]");

        /* Sprint 118: C99 width mark (type-tag verifier input). The mark's
         * logical width is `w` (signed, w in {1,2,4,8}) or `w-8` (unsigned,
         * w in {9,10,12}); exotic bit-field marks (e.g. 40/48) are printed
         * raw. OP_OVF_TRAP reuses the field as CHECK metadata. Ops that
         * reuse .width for call/asm payload metadata are excluded. */
        if (instr->width != 0 && instr->op != OP_CALL && instr->op != OP_CALL_ARG &&
            instr->op != OP_ASM && instr->op != OP_ASM_ARG) {
            fprintf(f, " [w=%u]", instr->width);
        }
        if ((instr->op == OP_DIV || instr->op == OP_MOD) && instr->cond != 0)
            fprintf(f, " [u]");
        fprintf(f, "\n");
    }

    /* Sprint 118: per-temp value-type tag table (type-tag verifier input).
     * Only tagged temps are listed; NONE (0) is omitted. */
    if (c->temp_type_tags && c->temp_type_tags_hi > 0) {
        fprintf(f, "# temp tags:");
        for (uint32_t id = 0; id < c->temp_type_tags_hi && id < 65536; id++) {
            uint8_t tag = c->temp_type_tags[id];
            if (tag == 0) continue;
            const char *cls = zc_tag_class(tag) == ZC_TAGCLS_INT ? "int" :
                              zc_tag_class(tag) == ZC_TAGCLS_UINT ? "uint" :
                              zc_tag_class(tag) == ZC_TAGCLS_FLOAT ? "float" :
                              zc_tag_class(tag) == ZC_TAGCLS_INT128 ? "int128" :
                              zc_tag_class(tag) == ZC_TAGCLS_UINT128 ? "uint128" :
                              zc_tag_class(tag) == ZC_TAGCLS_COMPLEX ? "complex" :
                              zc_tag_class(tag) == ZC_TAGCLS_VECTOR ? "vector" :
                              zc_tag_class(tag) == ZC_TAGCLS_PTR ? "ptr" : "?";
            if (zc_tag_class(tag) == ZC_TAGCLS_PTR)
                fprintf(f, " r%u=%s", (unsigned)id, cls);
            else
                fprintf(f, " r%u=%s,%d", (unsigned)id, cls, zc_tag_width_bytes(tag));
        }
        fprintf(f, "\n");
    }
}
