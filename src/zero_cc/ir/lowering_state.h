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

    /* Float argument spill offsets per call index (indices beyond the FP
     * register count are the stack-passed FP args, pushed by the call site) */
    int32_t call_fp_spills[64][16];
    int call_fp_spill_counts[64];
    int call_count;
} IREmitterContext;

/* VLA State - Hybrid VLA enforcement (single-scope, no nested, no loops) */
typedef struct {
    bool has_runtime_vla;              /* Function has runtime VLA */
    int vla_count;                     /* Number of runtime VLA in function */
    int block_depth;                   /* Current block nesting depth */
    int loop_depth;                    /* Current loop nesting depth */
    int32_t cur_sp_slot_disp;          /* Frame disp of saved-SP slot for current OP_VLA_ALLOC */
} VLAState;

/* Lowering State - Encapsulated per-compiler-instance state for Task 61/62 */
typedef struct {
    size_t current_func_idx;           /* Index of function currently being lowered */
    IREmitterContext ir_ctx;           /* IR emission state (call tracking) */
    uint16_t epilogue_label;           /* Synthetic epilogue label for returns */
    bool is_frameless;                 /* True when leaf function needs no stack frame */
    bool has_stack_protector;          /* True when function has stack guard canary enabled */
    int32_t canary_stack_off;          /* Frame slot offset (positive, fp-relative) holding canary */
    bool omit_frame_pointer;           /* Option 4: frame pointer omitted; frame slots are
                                          addressed from the stack pointer instead. Only set
                                          for functions with a fully static frame (no VLA). */
    bool has_frame_address;            /* True when function uses __builtin_frame_address or
                                          __builtin_return_address; forces a real frame pointer */
    bool has_setjmp;                   /* True when function uses __builtin_setjmp/longjmp;
                                          forces a frame pointer AND disables mem2reg so the
                                          automatics stay in stack slots (restored by longjmp) */
    bool has_capture_mirror;           /* Phase 3a: nested function with by-reference
                                          captures; the mirror locals must stay in stack
                                          slots (mirror-in/out access the slots directly),
                                          so mem2reg is skipped for the whole function */
    int32_t frame_delta;               /* Displacement delta added to FP-relative offsets when
                                          addressing frame slots from SP (0 when the frame
                                          pointer is present). Equals the ISA prologue's
                                          (FP - final_SP) distance. In _Alignas realign mode
                                          (frame_align > 16) it equals the N-rounded frame
                                          size: the frame is anchored to the realigned SP'. */
    int32_t frame_align;               /* C11 _Alignas(N>16) local: prologue realigns SP to N
                                          and the N-rounded frame is anchored to it (x86-64:
                                          and $-N, rsp; aarch64: mask through x16; riscv:
                                          andi). 0/16 = ordinary 16-rounded frame. N is a
                                          power of two <= 4096. */
    int32_t sp_snap_off;               /* Sprint 99: frame slot holding a snapshot of the
                                          body-time SP, taken in the prologue. Call sites of
                                          nested functions with nonlocal-goto pairs load it to
                                          pass the enclosing scope's final SP (mode-agnostic:
                                          OFP / red-zone / realign all just read the stored
                                          value). 0 = unused. */
    int32_t fp_snap_off;               /* Sprint 99: frame slot holding a snapshot of the
                                          entry-time frame pointer (rbp/x29/s0), taken in the
                                          prologue while it still holds the caller's value
                                          (OFP prologues never touch it). A frame-pointer-
                                          omitted function that is a nonlocal-goto target
                                          re-establishes this value at each landing point, so
                                          the ABI-visible FP register is preserved across the
                                          foreign jump. 0 = unused. */
    int static_local_rt_off;           /* Frame slot (positive, fp-relative) that
                                          protects VREG_0 across the epilogue
                                          static-local mirror-out; 0 = unused */
    int static_local_rt2_off;          /* Second slot: protects VREG_2 across the
                                          same mirror-out - 9-16B integer struct
                                          returns live in VREG_0/VREG_2 (rax/rdx),
                                          and an allocator temp mapped onto VREG_2
                                          during the mirror-out clobbers the second
                                          return word (c-torture execute/pr92618) */

    /* CMP operand tracking for RISC-V backend (flags materialization):
     * RISC-V has no flags register, so the last OP_CMP/OP_CMP_ZERO's
     * operands (and class) are recorded here; the flag consumer
     * (OP_COND_SET / OP_JCOND) re-materializes the predicate from them.
     * For OP_CMP_ZERO, cmp_src2 is 0 (the zero register). */
    int cmp_src1;                      /* First operand of last CMP instruction */
    int cmp_src2;                      /* Second operand of last CMP instruction */
    int cmp_is_fp;                     /* Last CMP had FP-class operands */
    int div_is_unsigned;               /* Current DIV/MOD uses unsigned division */

    /* Hybrid VLA enforcement state */
    VLAState vla;                      /* VLA tracking and enforcement */

    /* GNU variably-modified struct definitions: the member dimension
     * expressions are evaluated when the definition executes (GCC
     * semantics - later changes to the dimension variables do not affect
     * the object's size). Each entry maps a struct tag to 8-byte frame
     * slots holding the captured dimensions (one per VLA member, in
     * member order); NODE_VLA_DIM_SLOT loads from these slots. */
    struct ZeroCCVlaStructCapture {
        char tag[64];
        int32_t slots[8];
        int count;
    } vla_struct_captures[8];
    int vla_struct_capture_count;

    /* Alias group tracking for restrict pointers (temp -> alias_group ID) */
    uint8_t temp_alias_group[256];

    /* Function Inlining State */
    struct {
        int active_depth;              /* Current inlining recursion depth */
        int return_target_temp;        /* Destination temp for inlined returns */
        uint16_t return_target_label;  /* Exit label for inlined returns (0 = none) */
        ZeroCCType return_type;        /* Declared return type of inlined callee */
        int param_count;               /* Number of inlined parameters */
        const char *param_names[8];    /* Parameter names */
        int param_temps[8];            /* Evaluated argument temps */
        bool param_is_func_ptr[8];     /* Param is a function pointer (indirect calls through it must stay indirect) */
        bool param_is_128[8];          /* Sprint 110: param is a 128-bit scalar (GPR128 temp) - param substitution must set last_expr_is_128 */
    } inlining;
} LoweringState;

#endif /* ZERO_CC_PARSER_LOWERING_STATE_H */
