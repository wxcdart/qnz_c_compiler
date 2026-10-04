/* Lowering State - Encapsulated per-compiler-instance state for Task 61
 *
 * This header defines the LoweringState struct that encapsulates all
 * state previously stored in global variables, enabling reentrant compilation.
 */

#ifndef ZERO_CC_PARSER_LOWERING_STATE_H
#define ZERO_CC_PARSER_LOWERING_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
typedef struct {
    int temp_id;
    size_t live_start;
    size_t live_end;
    int reg_class;
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

/* Lowering State - Encapsulated per-compiler-instance state for Task 61 */
typedef struct {
    IREmitterContext ir_ctx;                      /* IR emission state */
    uint16_t epilogue_label;                      /* Synthetic epilogue label for returns */
    struct {                                      /* Loop control flow stack */
        uint16_t break_label;
        uint16_t continue_label;
    } loop_stack[16];
    int loop_depth;
    struct {                                      /* User-defined label table */
        char name[64];
        uint16_t ir_label;
    } user_labels[256];
    int user_label_count;
} LoweringState;

#endif /* ZERO_CC_PARSER_LOWERING_STATE_H */
