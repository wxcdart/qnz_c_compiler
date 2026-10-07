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

/* IR Emitter Context - holds state for IR emission (Task 62: simplified) */
typedef struct {
    TempVar temp_vars[256];
    size_t temp_var_count;
    int next_temp_id;
    
    /* Float argument spill offsets per call index */
    int32_t call_fp_spills[64][8];
    int call_fp_spill_counts[64];
    int call_count;
} IREmitterContext;

/* Lowering State - Encapsulated per-compiler-instance state for Task 61/62 */
typedef struct {
    IREmitterContext ir_ctx;           /* IR emission state (call tracking) */
    uint16_t epilogue_label;           /* Synthetic epilogue label for returns */
    
    /* CMP operand tracking for RISC-V backend (flags materialization) */
    int cmp_src1;                      /* First operand of last CMP instruction */
    int cmp_src2;                      /* Second operand of last CMP instruction */
} LoweringState;

#endif /* ZERO_CC_PARSER_LOWERING_STATE_H */
