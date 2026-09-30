#ifndef ZERO_CC_ISA_BACKEND_H
#define ZERO_CC_ISA_BACKEND_H

#include <stdint.h>
#include <stddef.h>

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

/* Emit backend - instruction generation */
typedef struct ZeroCCISABackend {
    ZeroCCISA isa;
    const ISARegInfo *regs;
    
    /* Function prologue/epilogue */
    void (*emit_prologue)(ZeroCCCompiler *c, size_t stack_size);
    void (*emit_epilogue)(ZeroCCCompiler *c);
    
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
    void (*emit_neg)(ZeroCCCompiler *c, int dst, int src);
    void (*emit_test)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_cqo)(ZeroCCCompiler *c);
    void (*emit_idiv)(ZeroCCCompiler *c, int src);
    void (*emit_lea)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    size_t (*patch_jmp)(ZeroCCCompiler *c, size_t patch_offset, size_t target_offset);
    
    /* Load/store */
    void (*emit_load)(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store)(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    
    /* Control flow */
    size_t (*emit_jmp)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jeq)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jne)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jlt)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jle)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jgt)(ZeroCCCompiler *c, const char *label);
    size_t (*emit_jge)(ZeroCCCompiler *c, const char *label);
    void (*emit_call)(ZeroCCCompiler *c, const char *func_name);
    void (*emit_ret)(ZeroCCCompiler *c);
    
    /* Floating point */
    void (*emit_load_fp)(ZeroCCCompiler *c, int dst_reg, int base_reg, int32_t offset);
    void (*emit_store_fp)(ZeroCCCompiler *c, int src_reg, int base_reg, int32_t offset);
    void (*emit_load_fp_imm)(ZeroCCCompiler *c, int dst_reg, double value);
    void (*emit_fp_add)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_sub)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_mul)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_div)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_fp_neg)(ZeroCCCompiler *c, int dst, int src);
    void (*emit_fp_cmp)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_fp_cvtsi)(ZeroCCCompiler *c, int int_dst, int fp_src);  /* float->int */
    void (*emit_fp_cvtsf)(ZeroCCCompiler *c, int fp_dst, int int_src);  /* int->float */
    void (*emit_fp_mov)(ZeroCCCompiler *c, int dst, int src);           /* reg to reg */
    
    /* Immediate values */
    void (*emit_const)(ZeroCCCompiler *c, int64_t value);
    
    /* Stack frame */
    void (*emit_stack_adjust)(ZeroCCCompiler *c, int32_t delta);
    void (*emit_push_reg)(ZeroCCCompiler *c, int reg);
    void (*emit_pop_reg)(ZeroCCCompiler *c, int reg);
    
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
const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa);
const ZeroCCISABackend *zero_cc_get_native_backend(void);

/* Convenience macro for compiler access */
#define BE(c) ((c)->isa_backend)

#endif /* ZERO_CC_ISA_BACKEND_H */