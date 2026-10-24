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
    const char *vec_regs[32];      /* Vector registers (xmm/v/vr) */
    int num_vec_regs;              /* Number of vector registers */
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
    const char *(*get_vec_reg_name)(int vec_reg_idx);  /* Get vector register name */

    /* Assembly emission for IR instructions */
    void (*emit_mov_imm_asm)(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
    void (*emit_mov_reg_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_load_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_store_asm)(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
    void (*emit_add_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_sub_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_mul_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_div_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_mod_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_and_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_or_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_xor_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_shl_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_shr_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_sar_asm)(ZeroCCCompiler *c, int dst, int src, int shift, FILE *out);
    void (*emit_extend_asm)(ZeroCCCompiler *c, int dst, int src, int width, int sign, FILE *out); /* width 1/2/4, sign 0=zext 1=sext */
    void (*emit_cmp_asm)(ZeroCCCompiler *c, int src1, int src2, FILE *out);
    void (*emit_cond_set_asm)(ZeroCCCompiler *c, int dst, int cond, FILE *out);
    void (*emit_neg_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    /* Sprint 40: in-place ALU ops previously fell through to the
     * `# UNKNOWN` default in asm_emitter.c (the -S output was silently
     * wrong for any IR containing them). */
    void (*emit_add_imm_asm)(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
    void (*emit_sub_imm_asm)(ZeroCCCompiler *c, int dst, int64_t imm, FILE *out);
    void (*emit_inc_asm)(ZeroCCCompiler *c, int dst, FILE *out);
    void (*emit_dec_asm)(ZeroCCCompiler *c, int dst, FILE *out);
    void (*emit_not_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_lea_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_deref_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_call_asm)(ZeroCCCompiler *c, const char *func_name, int arg_count, int *arg_temps, int *is_fp, FILE *out);
    void (*emit_ret_asm)(ZeroCCCompiler *c, FILE *out);
    void (*emit_label_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jmp_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jeq_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jne_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jlt_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jle_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jgt_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_jge_asm)(ZeroCCCompiler *c, const char *label, FILE *out);
    void (*emit_load_fp_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_store_fp_asm)(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
    void (*emit_fp_add_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_sub_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_mul_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_fp_div_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    /* FP compare (ucomisd / fcmp) for OP_CMP with float operands, followed
     * by the standard emit_cond_set_asm. NULL on backends without an asm
     * float-flags mechanism (RISC-V: no flags register). */
    void (*emit_fp_cmp_asm)(ZeroCCCompiler *c, int src1, int src2, FILE *out);
    void (*emit_fp_mov_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_fp_cvtsf_asm)(ZeroCCCompiler *c, int fp_dst, int int_src, FILE *out);
    void (*emit_fp_cvtsi_asm)(ZeroCCCompiler *c, int int_dst, int fp_src, FILE *out);
    void (*emit_vla_alloc_asm)(ZeroCCCompiler *c, int dst, int size, FILE *out);
    void (*emit_param_save_int_asm)(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out);
    void (*emit_param_save_fp_asm)(ZeroCCCompiler *c, int reg_idx, int32_t offset, FILE *out);
    void (*emit_variadic_spill_asm)(ZeroCCCompiler *c, int param_count, int32_t base_offset, FILE *out);
    /* Like emit_call_asm: arg_temps are temp ids (number first), resolved
     * against the register allocation by the backend. */
    void (*emit_syscall_asm)(ZeroCCCompiler *c, int arg_count, const int *arg_temps, FILE *out);

    /* Vector instruction emission (SSE2/NEON/RVV) */
    void (*emit_vload_asm)(ZeroCCCompiler *c, int dst, int base, int32_t offset, FILE *out);
    void (*emit_vstore_asm)(ZeroCCCompiler *c, int src, int base, int32_t offset, FILE *out);
    void (*emit_vaddss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vsubss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vmulss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vdivss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vaddps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vsubps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vmulps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vdivps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vaddsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vsubsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vmulsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vdivsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vaddpd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vsubpd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vmulpd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vdivpd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vpaddd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vpsubd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vpmulld_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vmovss_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vmovsd_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vmovaps_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vmovapd_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vandps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vorps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vxorps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, FILE *out);
    void (*emit_vshufps_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int imm, FILE *out);
    void (*emit_vcvt_si2ss_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vcvt_ss2si_asm)(ZeroCCCompiler *c, int dst, int src, FILE *out);
    void (*emit_vcmpss_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out);
    void (*emit_vcmpsd_asm)(ZeroCCCompiler *c, int dst, int src1, int src2, int pred, FILE *out);

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
    void (*emit_extend)(ZeroCCCompiler *c, int dst, int src, int width, int sign); /* width 1/2/4, sign 0=zext 1=sext, in-place */
    void (*emit_test)(ZeroCCCompiler *c, int src1, int src2);
    void (*emit_cqo)(ZeroCCCompiler *c);
    void (*emit_idiv)(ZeroCCCompiler *c, int src);
    void (*emit_lea)(ZeroCCCompiler *c, int dst, int base, int32_t offset);

    /* Vector operations for binary JIT emission */
    void (*emit_vload)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    void (*emit_vstore)(ZeroCCCompiler *c, int src, int base, int32_t offset);
    /* Scalar float load/store variants (movss/movsd) for OP_VLOADSS/SD,
     * OP_VSTORESS/SD - 4-byte and 8-byte float accesses through an address.
     * (The 128-bit emit_vload/emit_vstore are for OP_VLOAD/OP_VSTORE only.) */
    void (*emit_vloadss)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    void (*emit_vloadsd)(ZeroCCCompiler *c, int dst, int base, int32_t offset);
    void (*emit_vstoress)(ZeroCCCompiler *c, int src, int base, int32_t offset);
    void (*emit_vstoredsd)(ZeroCCCompiler *c, int src, int base, int32_t offset);
    void (*emit_vaddps)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vsubps)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vmulps)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vdivps)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vaddpd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vsubpd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vmulpd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vdivpd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vpaddd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vpsubd)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vpmulld)(ZeroCCCompiler *c, int dst, int src1, int src2);
    void (*emit_vmovaps)(ZeroCCCompiler *c, int dst, int src);

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
    /* OS syscall (OP_SYSCALL / __zerocc_syscall): arg_count temps (syscall
     * number first, then up to 6 operands) are resolved and moved into the
     * target's syscall argument registers one at a time, then the syscall
     * instruction is issued. The result is left in the ABI return register
     * (VREG_0). NULL on targets without syscall support yet (the dispatcher
     * reports a compile error). */
    void (*emit_syscall)(ZeroCCCompiler *c, int arg_count, const int *arg_temps);
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

    /* Scratch registers for spilling (ISA-specific, non-argument/return registers) */
    int scratch_reg1;  /* First scratch register (e.g., r10 on x86, x9 on AArch64, t0 on RISC-V) */
    int scratch_reg2;  /* Second scratch register (e.g., r11 on x86, x10 on AArch64, t1 on RISC-V) */
    int fp_scratch_reg1; /* FP scratch for spilled FP src1/dest (outside the 0-7 XMM pool) */
    int fp_scratch_reg2; /* FP scratch for spilled FP src2 */

    /* Callee-save frame reserve in bytes (ISA-specific: 48 for x86, 96 for AArch64/RISC-V) */
    int callee_save_frame_reserve;
} ZeroCCISABackend;

/* Backend registry */
extern ZeroCCISABackend x86_64_backend;
extern ZeroCCISABackend aarch64_backend;
extern ZeroCCISABackend riscv64_backend;
extern ZeroCCISABackend riscv32_backend;
const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa);
const ZeroCCISABackend *zero_cc_get_native_backend(void);

/* Convenience macro for compiler access */
#define BE(c) ((c)->isa_backend)

/* Frame slot addressing (Option 4: -fomit-frame-pointer). Frame-slot IR
 * operands (OP_MOV / OP_MOV_MEM / OP_LEA / OP_FLOAD / OP_FSTORE /
 * OP_PARAM_SAVE_* / spills / callee-save slots) are recorded FP-relative.
 * When the frame pointer is omitted for a fixed-frame function, slots are
 * addressed from SP with a constant delta (LoweringState.frame_delta);
 * these accessors keep every emission site uniform. Implemented in ir.c. */
int zero_cc_frame_base_reg(const ZeroCCCompiler *c);   /* fp_reg or sp_reg */
int32_t zero_cc_frame_disp(const ZeroCCCompiler *c, int32_t off); /* off + delta */

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

/* ASM emission initialization functions */
void x86_init_asm_emission(ZeroCCISABackend *be);
void aarch64_init_asm_emission(ZeroCCISABackend *be);
void riscv64_init_asm_emission(ZeroCCISABackend *be);

#endif /* ZERO_CC_ISA_BACKEND_H */
