/* IR Emitter - IR Instruction Emission API and State Management */
#ifndef ZERO_CC_PARSER_IR_EMITTER_H
#define ZERO_CC_PARSER_IR_EMITTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "zero_cc/types/types.h"
#include "ir.h"
#include "regalloc.h"
#include "live_range.h"

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Temp variable tracking for register allocation */
typedef struct {
    int temp_id;
    size_t live_start;
    size_t live_end;
    RegClass reg_class;
    int assigned_reg;
    int spill_slot;
} TempVar;

/* IR Emitter Context - holds state for IR emission */
typedef struct {
    TempVar temp_vars[256];
    size_t temp_var_count;
    int next_temp_id;
    
    /* Loop control flow */
    size_t break_patch_pos[16][64];
    int break_patch_count[16];
    size_t continue_patch_pos[16][64];
    int continue_patch_count[16];
    size_t loop_start_pos[16];
    int loop_depth;
    
    /* Label/Goto support */
    struct {
        char name[64];
        size_t position;
    } label_table[256];
    int label_count;
    size_t* goto_patch_list[256];
    int goto_patch_count[256];
    int goto_patch_list_count;
    
    size_t epilogue_patches[32];
    int epilogue_patch_count;
    
    /* Float argument spill offsets per call index */
    int32_t call_fp_spills[64][8];
    int call_fp_spill_counts[64];
    int call_count;
} IREmitterContext;

/* Initialize IR emitter context */
static inline void ir_emitter_init(IREmitterContext *ctx) {
    ctx->temp_var_count = 0;
    ctx->next_temp_id = 0;
    ctx->loop_depth = 0;
    ctx->label_count = 0;
    ctx->goto_patch_list_count = 0;
    ctx->epilogue_patch_count = 0;
    ctx->call_count = 0;
    for (int i = 0; i < 64; i++) {
        ctx->call_fp_spill_counts[i] = 0;
    }
    for (int i = 0; i < 16; i++) {
        ctx->break_patch_count[i] = 0;
        ctx->continue_patch_count[i] = 0;
    }
    for (int i = 0; i < 256; i++) {
        ctx->goto_patch_count[i] = 0;
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

/* Label management */
static inline int ir_emitter_find_label(IREmitterContext *ctx, const char *name) {
    for (int i = 0; i < ctx->label_count; i++) {
        if (strcmp(ctx->label_table[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static inline int ir_emitter_add_label(IREmitterContext *ctx, const char *name, size_t position) {
    if (ctx->label_count < 256) {
        strncpy(ctx->label_table[ctx->label_count].name, name, 63);
        ctx->label_table[ctx->label_count].name[63] = '\0';
        ctx->label_table[ctx->label_count].position = position;
        return ctx->label_count++;
    }
    return -1;
}

static inline void ir_emitter_reset_labels(IREmitterContext *ctx) {
    ctx->label_count = 0;
    ctx->goto_patch_list_count = 0;
    for (int i = 0; i < 256; i++) {
        ctx->goto_patch_count[i] = 0;
    }
}

/* Loop control flow management */
static inline void ir_emitter_enter_loop(IREmitterContext *ctx, size_t loop_start) {
    if (ctx->loop_depth < 16) {
        ctx->loop_start_pos[ctx->loop_depth] = loop_start;
        ctx->break_patch_count[ctx->loop_depth] = 0;
        ctx->continue_patch_count[ctx->loop_depth] = 0;
        ctx->loop_depth++;
    }
}

static inline void ir_emitter_exit_loop(IREmitterContext *ctx) {
    if (ctx->loop_depth > 0) {
        ctx->loop_depth--;
    }
}

static inline size_t ir_emitter_get_loop_start(IREmitterContext *ctx) {
    if (ctx->loop_depth > 0) {
        return ctx->loop_start_pos[ctx->loop_depth - 1];
    }
    return 0;
}

static inline void ir_emitter_add_break_patch(IREmitterContext *ctx, size_t patch_pos) {
    if (ctx->loop_depth > 0) {
        int depth = ctx->loop_depth - 1;
        if (ctx->break_patch_count[depth] < 64) {
            ctx->break_patch_pos[depth][ctx->break_patch_count[depth]++] = patch_pos;
        }
    }
}

static inline void ir_emitter_add_continue_patch(IREmitterContext *ctx, size_t patch_pos) {
    if (ctx->loop_depth > 0) {
        int depth = ctx->loop_depth - 1;
        if (ctx->continue_patch_count[depth] < 64) {
            ctx->continue_patch_pos[depth][ctx->continue_patch_count[depth]++] = patch_pos;
        }
    }
}

#endif /* ZERO_CC_PARSER_IR_EMITTER_H */