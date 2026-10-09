#ifndef ZERO_CC_ISA_BACKEND_H
#define ZERO_CC_ISA_BACKEND_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;
typedef struct ZeroCCType ZeroCCType;

typedef enum {
    ISA_X86_64,
    ISA_AARCH64,
    ISA_RISCV32,
    ISA_RISCV64,
    ISA_XTENSA,
} ZeroCCISA;

/* Condition codes for OP_CMP_BRANCH */
#define CC_LT  0  /* signed less than */
#define CC_LE  1  /* signed less or equal */
#define CC_GT  2  /* signed greater */
#define CC_GE  3  /* signed greater or equal */
#define CC_EQ  4  /* equal */
#define CC_NE  5  /* not equal */

/* Register naming */
typedef struct {
    const char *int_regs[32];
    const char *fp_regs[32];
    const char *int_arg_regs[8];
    const char *fp_arg_regs[8];
    const char *int_ret_reg;
    const char *fp_ret_reg;
    int num_int_regs;
    int num_fp_regs;
} ISARegInfo;

/* Calling convention tables - ISA-specific */
typedef struct {
    const int *int_arg_regs;      /* Array of int arg register indices */
    int int_arg_reg_count;        /* Number of int arg registers */
    const int *fp_arg_regs;       /* Array of FP arg register indices */
    int fp_arg_reg_count;         /* Number of FP arg registers */
    const int *callee_saved_gpr;  /* Array of callee-saved GPR indices */
    int callee_saved_gpr_count;   /* Number of callee-saved GPRs */
    const int32_t *callee_save_slot_offsets; /* Slot offsets for each callee-saved GPR (indexed by array position) */
} ISACallingConvention;

/* Emit backend - instruction generation */
typedef struct ZeroCCISABackend {
    ZeroCCISA isa;
    const ISARegInfo *regs;
    const ISACallingConvention *conv;  /* Calling convention tables */
    
    /* Function prologue/epilogue */
    void (*emit_prologue)(ZeroCCCompiler *c, size_t stack_size, uint32_t callee_save_mask);
    void (*emit_leave)(ZeroCCCompiler *c);
    void (*emit_epilogue)(ZeroCCCompiler *c);
    size_t (*get_code_offset)(ZeroCCCompiler *c);
    
    /* Assembly emission (text output) */
    void (*emit_prologue_asm)(ZeroCCCompiler *c, size_t stack_size);
    void (*emit_epilogue_asm)(ZeroCCCompiler *c);
    const char *(*get_hw_reg_name)(int phys_reg);  /* Get register name by hardware number */
    
    /* Assembly emission for IR instructions */
    void (*emit_mov_imm_asm)(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
    void (*emit_mov_reg_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_load_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_store_asm)(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
    void (*emit_add_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_sub_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_mul_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_div_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_and_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_or_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_xor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_shl_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_shr_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_sar_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_cmp_asm)(ZeroCCCompiler *c, int src1, int src2, FILE *out);
    void (*emit_cond_set_asm)(ZeroCCCompiler *c, int dst, int cond, FILE *out);
    void (*emit_neg_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_lea_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_deref_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_call_asm)(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, FILE *out);
    void (*emit_ret_asm)(ZeroCCCompiler *c, FILE *out);
    void (*emit_label_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jmp_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jeq_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jne_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jlt_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jle_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jgt_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jge_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_fp_add_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_sub_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_mul_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_div_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_mov_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vla_alloc_asm)(ZeroCCCompiler *c, int dst, int size, FILE *out);
    
    /* Integer operations */
    void (*emit_mov_imm)(ZeroCCCompiler *c, int dst_reg, int64_t imm);
    void (*emit_add)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_sub)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_mul)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_div)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_mod)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_and)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_or)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_xor)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_shl)(ZeroCCCompiler *c, int dst, int src, int shift);
    void (*emit_cmp)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_cond_set)(ZeroCCCompiler *c, int dst, int cond);
    void (*emit_neg)(ZeroCCCompiler *c, int dst, int src);
    void (*emit_test)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_cqo)(ZeroCCCompiler *c);
    void (*emit_idiv)(ZeroCCCompiler *c, int src);
    void (*emit_lea)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    size_t (*patch_jmp)(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset);
    void (*resolve_labels)(ZeroCCCompiler *c);  /* Resolve all label patches */
    
    /* Load/store */
    void (*emit_load)(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store)(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    
    /* Additional operations for IR-based codegen */
    void (*emit_mov_reg)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_push_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_pop_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_add_imm)(ZeroCCCompiler *c, int dst_reg, int16_t imm);
    void (*emit_sub_imm)(ZeroCCCompiler *c, int dst_reg, int16_t imm);
    void (*emit_inc)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_dec)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_shr)(ZeroCCCompiler *c, int dst_reg, int src1, int src2);
    void (*emit_sar)(ZeroCCCompiler *c, int dst_reg, int src1, int src2);
    void (*emit_setl)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_setle)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_setg)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_setge)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_sete)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_setne)(ZeroCCCompiler *c, int dst_reg);
    void (*emit_deref)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_load_byte)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_load_16)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_load_32)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_store_byte)(ZeroCCCompiler *c, int src_reg, int base_reg);
    void (*emit_store_16)(ZeroCCCompiler *c, int src_reg, int base_reg);
    void (*emit_store_32)(ZeroCCCompiler *c, int src_reg, int base_reg);
    void (*emit_store_64)(ZeroCCCompiler *c, int src_reg, int base_reg);
    void (*emit_vla_alloc)(ZeroCCCompiler *c, int dst_reg, int size_reg);
    void (*emit_atomic_xadd)(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
    void (*emit_atomic_cmpxchg)(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width);
    void (*emit_atomic_cmpxchg_bool)(ZeroCCCompiler *c, int dst_reg, int addr_reg, int expected_reg, int desired_reg, int width);
    void (*emit_atomic_xchg)(ZeroCCCompiler *c, int dst_reg, int addr_reg, int val_reg, int width);
    void (*emit_mfence)(ZeroCCCompiler *c);
    void (*emit_cmp_zero)(ZeroCCCompiler *c, int src_reg);
    void (*emit_not)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    void (*emit_lnot)(ZeroCCCompiler *c, int dst_reg, int src_reg);
    /* Control flow */
    size_t (*emit_jmp)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jeq)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jne)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jlt)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jle)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jgt)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jge)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jcond)(ZeroCCCompiler *c, int cond, const char *label);
    void (*emit_call)(ZeroCCCompiler *c, const char *func_name);
    void (*emit_call_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_ret)(ZeroCCCompiler *c);
    
    /* Floating point */
    void (*emit_load_fp)(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store_fp)(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    void (*emit_load_fp_rodata)(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset);
    void (*emit_fp_add)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_sub)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_mul)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_div)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_neg)(ZeroCCCompiler *c, int dst, int src);
    void (*emit_fp_cmp)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_fp_cvtsi)(ZeroCCCompiler *c, int int_dst, int fp_src);  /* float->int */
    void (*emit_fp_cvtsf)(ZeroCCCompiler *c, int fp_dst, int int_src);  /* int->float */
    void (*emit_fp_mov)(ZeroCCCompiler *c, int dst, int src);           /* reg to reg */
    
    /* Parameter spilling / function setup */
    void (*emit_param_save_int)(ZeroCCCompiler *c, int reg_idx, int32_t offset);
    void (*emit_param_save_fp)(ZeroCCCompiler *c, int reg_idx, int32_t offset);
    void (*emit_variadic_spill)(ZeroCCCompiler *c, int param_count, int32_t base_offset);
    void (*emit_variadic_al_setup)(ZeroCCCompiler *c, int num_fp_args);
    
    /* RIP-relative addressing for string literals */
    void (*emit_lea_rip)(ZeroCCCompiler *c, int dst_reg, size_t rodata_offset);
    
    /* Immediate values */
    void (*emit_const)(ZeroCCCompiler *c, int64_t value);
    
    /* Stack frame */
    void (*emit_stack_adjust)(ZeroCCCompiler *c, int32_t delta);
    void (*emit_stack_align)(ZeroCCCompiler *c, uint8_t alignment);

    /* Labels */
    void (*emit_label)(ZeroCCCompiler *c, const char *name);
    
    /* VREG mapping (ISA-independent virtual to physical register) */
    int (*get_int_reg)(int vreg);  /* Map VREG_INT_* to physical */
    int (*get_fp_reg)(int vreg);   /* Map VREG_FP_* to physical */
    
    /* Frame pointer */
    int fp_reg;        /* Frame pointer register index */
    int sp_reg;        /* Stack pointer register index */
} ZeroCCISABackend;

/* Backend registry */
extern const ZeroCCISABackend x86_64_backend;
extern const ZeroCCISABackend aarch64_backend;
extern const ZeroCCISABackend riscv64_backend;
extern const ZeroCCISABackend riscv32_backend;
const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa);
const ZeroCCISABackend *zero_cc_get_native_backend(void);

/* Convenience macro for compiler access */
#define BE(c) ((c)->isa_backend)

/* Helper functions for calling convention access */
static inline int backend_get_int_arg_reg(const ZeroCCISABackend *be, int idx) {
    if (be->conv && idx < be->conv->int_arg_reg_count) {
        return be->conv->int_arg_regs[idx];
    }
    return -1;  /* Stack-passed */
}

static inline int backend_get_fp_arg_reg(const ZeroCCISABackend *be, int idx) {
    if (be->conv && idx < be->conv->fp_arg_reg_count) {
        return be->conv->fp_arg_regs[idx];
    }
    return -1;  /* Stack-passed */
}

#endif /* ZERO_CC_ISA_BACKEND_H */