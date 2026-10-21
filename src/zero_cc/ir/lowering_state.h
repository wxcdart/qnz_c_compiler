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

/* VLA State - Hybrid VLA enforcement (single-scope, no nested, no loops) */
typedef struct {
    bool has_runtime_vla;              /* Function has runtime VLA */
    int vla_count;                     /* Number of runtime VLA in function */
    int block_depth;                   /* Current block nesting depth */
    int loop_depth;                    /* Current loop nesting depth */
} VLAState;

/* Lowering State - Encapsulated per-compiler-instance state for Task 61/62 */
typedef struct {
    IREmitterContext ir_ctx;           /* IR emission state (call tracking) */
    uint16_t epilogue_label;           /* Synthetic epilogue label for returns */
    int static_local_rt_off;           /* Frame slot (positive, fp-relative) that
                                          protects VREG_0 across the epilogue
                                          static-local mirror-out; 0 = unused */
    
    /* CMP operand tracking for RISC-V backend (flags materialization):
     * RISC-V has no flags register, so the last OP_CMP/OP_CMP_ZERO's
     * operands (and class) are recorded here; the flag consumer
     * (OP_COND_SET / OP_JCOND) re-materializes the predicate from them.
     * For OP_CMP_ZERO, cmp_src2 is 0 (the zero register). */
    int cmp_src1;                      /* First operand of last CMP instruction */
    int cmp_src2;                      /* Second operand of last CMP instruction */
    int cmp_is_fp;                     /* Last CMP had FP-class operands */
    
    /* Hybrid VLA enforcement state */
    VLAState vla;                      /* VLA tracking and enforcement */
    
    /* Alias group tracking for restrict pointers (temp -> alias_group ID) */
    uint8_t temp_alias_group[256];

    /* Function Inlining State */
    struct {
        int active_depth;              /* Current inlining recursion depth */
        int return_target_temp;        /* Destination temp for inlined returns */
        uint16_t return_target_label;  /* Exit label for inlined returns (0 = none) */
        int param_count;               /* Number of inlined parameters */
        const char *param_names[8];    /* Parameter names */
        int param_temps[8];            /* Evaluated argument temps */
    } inlining;
} LoweringState;

#endif /* ZERO_CC_PARSER_LOWERING_STATE_H */
