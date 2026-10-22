/* IR Emitter - IR Instruction Emission API and State Management */
#ifndef ZERO_CC_PARSER_IR_EMITTER_H
#define ZERO_CC_PARSER_IR_EMITTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "zero_cc/types/types.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/ir/live_range.h"
#include "zero_cc/ir/lowering_state.h"

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Initialize IR emitter context */
static inline void ir_emitter_init(IREmitterContext *ctx) {
    ctx->temp_var_count = 0;
    ctx->next_temp_id = 0;
    ctx->call_count = 0;
    for (int i = 0; i < 64; i++) {
        ctx->call_fp_spill_counts[i] = 0;
    }
}

/* Allocate a new temporary variable */
static inline int ir_emitter_alloc_temp(IREmitterContext *ctx, RegClass reg_class, size_t live_start) {
    int temp_id = ctx->next_temp_id++;
    if (ctx->temp_var_count < 256) {
        TempVar *tv = &ctx->temp_vars[ctx->temp_var_count++];
        tv->temp_id = temp_id;
        tv->live_start = live_start;
        tv->live_end = 0;
        tv->reg_class = reg_class;
        tv->assigned_reg = -1;
        tv->spill_slot = -1;
    }
    return temp_id;
}

/* Find temp variable by ID */
static inline TempVar* ir_emitter_find_temp(IREmitterContext *ctx, int temp_id) {
    for (size_t i = 0; i < ctx->temp_var_count; i++) {
        if (ctx->temp_vars[i].temp_id == temp_id) {
            return &ctx->temp_vars[i];
        }
    }
    return NULL;
}

/* Check if temp is assigned to a register */
static inline bool ir_emitter_temp_is_reg(IREmitterContext *ctx, int temp_id) {
    TempVar *tv = ir_emitter_find_temp(ctx, temp_id);
    return tv && tv->assigned_reg >= 0;
}

/* Check if temp is spilled */
static inline bool ir_emitter_temp_is_spilled(IREmitterContext *ctx, int temp_id) {
    TempVar *tv = ir_emitter_find_temp(ctx, temp_id);
    return tv && tv->assigned_reg < 0;
}

/* Get assigned register for temp */
static inline int ir_emitter_get_temp_reg(IREmitterContext *ctx, int temp_id) {
    TempVar *tv = ir_emitter_find_temp(ctx, temp_id);
    return tv ? tv->assigned_reg : -1;
}

#endif /* ZERO_CC_PARSER_IR_EMITTER_H */
