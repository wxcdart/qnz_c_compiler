/* ISA backend registry (Sprint 114).
 *
 * Central lookup from a ZeroCCISA target to its ZeroCCISABackend vtable.
 * Lives in its own translation unit (extracted from x86_64.c) so that no
 * single ISA emitter owns the global registry - adding a target means
 * adding its vtable + one case here, without touching another ISA file.
 */
#include <stdbool.h>
#include "zero_cc/isa/isa_backend.h"
#include "zero_cc/isa/target_traits.h"
#include "zero_cc/ir/abi_classify.h"
#include "zero_cc/ir/ir.h"
#include "zero_cc/ir/ir_emit_ops.h"

static void generic_emit_variadic_setup(ZeroCCCompiler *c, int ap_addr, int32_t save_base,
                                        int gp_idx, int fp_idx, int stack_named) {
    const ZeroCCFunction *fn = (c && c->function_count > 0 && c->lowering.current_func_idx < c->function_count)
                                   ? &c->functions[c->lowering.current_func_idx] : NULL;
    bool is_ms = (c && c->isa == ISA_X86_64 && ((fn && fn->is_ms_abi) || c->is_win64_abi));
    if (is_ms) {
        int zero_t = zc_alloc_temp(c);
        ir_emit_mov_imm(c, zero_t, 0);
        ir_emit_store_64(c, zero_t, ap_addr);        /* +0 gp */
        int off8 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, off8, 8);
        int ap_8 = zc_alloc_temp(c);
        ir_emit_add(c, ap_8, ap_addr, off8);
        ir_emit_store_64(c, zero_t, ap_8);           /* +8 gp_end */
        int off16 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, off16, 16);
        int ap_16 = zc_alloc_temp(c);
        ir_emit_add(c, ap_16, ap_addr, off16);
        ir_emit_store_64(c, zero_t, ap_16);          /* +16 fp */
        int off24 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, off24, 24);
        int ap_24 = zc_alloc_temp(c);
        ir_emit_add(c, ap_24, ap_addr, off24);
        ir_emit_store_64(c, zero_t, ap_24);          /* +24 fp_end */

        bool uses_sret = fn && zero_cc_abi_type_uses_sret_fn(c, &fn->return_type, true);
        int named_count = (int)(fn ? fn->param_count : 0);
        int ovf_slot = (uses_sret ? 1 : 0) + named_count;
        int t = zc_alloc_temp(c);
        ir_emit_lea(c, t, TEMP_NONE, 16 + ovf_slot * 8);
        int off32 = zc_alloc_temp(c);
        ir_emit_mov_imm(c, off32, 32);
        int ap_32 = zc_alloc_temp(c);
        ir_emit_add(c, ap_32, ap_addr, off32);
        ir_emit_store_64(c, t, ap_32);               /* +32 ovf */
        return;
    }

    int t;
    int gp_regs = (c && BE(c) && BE(c)->conv) ? BE(c)->conv->int_arg_reg_count : 6;
    int fp_regs = (c && BE(c) && BE(c)->conv) ? BE(c)->conv->fp_arg_reg_count : 8;

    t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_idx * 8));
    ir_emit_store_64(c, t, ap_addr);             /* +0 gp */

    t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -(save_base + gp_regs * 8));
    int off8 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off8, 8);
    int ap_8 = zc_alloc_temp(c);
    ir_emit_add(c, ap_8, ap_addr, off8);
    ir_emit_store_64(c, t, ap_8);                /* +8 gp_end */

    t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_idx) * 8));
    int off16 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off16, 16);
    int ap_16 = zc_alloc_temp(c);
    ir_emit_add(c, ap_16, ap_addr, off16);
    ir_emit_store_64(c, t, ap_16);               /* +16 fp */

    t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -(save_base + (gp_regs + fp_regs) * 8));
    int off24 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off24, 24);
    int ap_24 = zc_alloc_temp(c);
    ir_emit_add(c, ap_24, ap_addr, off24);
    ir_emit_store_64(c, t, ap_24);               /* +24 fp_end */

    t = zc_alloc_temp(c);
    int ovf_start = zc_target_caller_arg_offset(c);
    int ovf_step = zc_target_slot_size(c);
    ir_emit_lea(c, t, TEMP_NONE, ovf_start + ovf_step * stack_named);
    int off32 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off32, 32);
    int ap_32 = zc_alloc_temp(c);
    ir_emit_add(c, ap_32, ap_addr, off32);
    ir_emit_store_64(c, t, ap_32);               /* +32 ovf */
}

static void aarch32_emit_variadic_setup(ZeroCCCompiler *c, int ap_addr, int32_t save_base,
                                        int gp_idx, int fp_idx, int stack_named) {
    (void)fp_idx;
    int t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -(save_base + 16 - gp_idx * 4));
    ir_emit_store_32(c, t, ap_addr);             /* +0 gp */

    t = zc_alloc_temp(c);
    ir_emit_lea(c, t, TEMP_NONE, -save_base);
    int off4 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off4, 4);
    int ap_4 = zc_alloc_temp(c);
    ir_emit_add(c, ap_4, ap_addr, off4);
    ir_emit_store_32(c, t, ap_4);                /* +4 gp_end */

    int off8 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off8, 8);
    int ap_8 = zc_alloc_temp(c);
    ir_emit_add(c, ap_8, ap_addr, off8);
    ir_emit_store_32(c, t, ap_8);                /* +8 fp */

    int off12 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off12, 12);
    int ap_12 = zc_alloc_temp(c);
    ir_emit_add(c, ap_12, ap_addr, off12);
    ir_emit_store_32(c, t, ap_12);               /* +12 fp_end */

    t = zc_alloc_temp(c);
    int ovf_start = 8;
    int ovf_step = 4;
    ir_emit_lea(c, t, TEMP_NONE, ovf_start + ovf_step * stack_named);
    int off16 = zc_alloc_temp(c);
    ir_emit_mov_imm(c, off16, 16);
    int ap_16 = zc_alloc_temp(c);
    ir_emit_add(c, ap_16, ap_addr, off16);
    ir_emit_store_32(c, t, ap_16);               /* +16 ovf */
}

static ABIStructClassification x86_64_classify_arg(const ZeroCCType *type) {
    return abi_classify_arg(ISA_X86_64, type);
}
static ABIStructClassification x86_64_classify_return(const ZeroCCType *type) {
    return abi_classify_return(ISA_X86_64, type);
}

static ABIStructClassification aarch64_classify_arg(const ZeroCCType *type) {
    return abi_classify_arg(ISA_AARCH64, type);
}
static ABIStructClassification aarch64_classify_return(const ZeroCCType *type) {
    return abi_classify_return(ISA_AARCH64, type);
}

static ABIStructClassification aarch32_classify_arg(const ZeroCCType *type) {
    return abi_classify_arg(ISA_AARCH32, type);
}
static ABIStructClassification aarch32_classify_return(const ZeroCCType *type) {
    return abi_classify_return(ISA_AARCH32, type);
}

static ABIStructClassification riscv64_classify_arg(const ZeroCCType *type) {
    return abi_classify_arg(ISA_RISCV64, type);
}
static ABIStructClassification riscv64_classify_return(const ZeroCCType *type) {
    return abi_classify_return(ISA_RISCV64, type);
}

static ABIStructClassification riscv32_classify_arg(const ZeroCCType *type) {
    return abi_classify_arg(ISA_RISCV32, type);
}
static ABIStructClassification riscv32_classify_return(const ZeroCCType *type) {
    return abi_classify_return(ISA_RISCV32, type);
}

/* Fill in ASM-emission function pointers once. The backend tables are
 * writable statics (not .rodata), so no const cast is needed; callers only
 * ever see them through const pointers. */
static void zero_cc_init_backends_once(void) {
    static bool done = false;
    if (done) return;
    riscv32_init_backend();  /* copy RV64 table + override RV32 fields (riscv32.c) */
    aarch32_init_backend();  /* copy AArch64 table + override A32 fields (aarch32.c) */
    x86_64_backend.traits = zero_cc_get_target_traits(ISA_X86_64);
    aarch64_backend.traits = zero_cc_get_target_traits(ISA_AARCH64);
    aarch32_backend.traits = zero_cc_get_target_traits(ISA_AARCH32);
    riscv64_backend.traits = zero_cc_get_target_traits(ISA_RISCV64);
    riscv32_backend.traits = zero_cc_get_target_traits(ISA_RISCV32);

    x86_64_backend.classify_arg = x86_64_classify_arg;
    x86_64_backend.classify_return = x86_64_classify_return;
    x86_64_backend.emit_variadic_setup = generic_emit_variadic_setup;
    aarch64_backend.classify_arg = aarch64_classify_arg;
    aarch64_backend.classify_return = aarch64_classify_return;
    aarch64_backend.emit_variadic_setup = generic_emit_variadic_setup;
    aarch32_backend.classify_arg = aarch32_classify_arg;
    aarch32_backend.classify_return = aarch32_classify_return;
    aarch32_backend.emit_variadic_setup = aarch32_emit_variadic_setup;
    riscv64_backend.classify_arg = riscv64_classify_arg;
    riscv64_backend.classify_return = riscv64_classify_return;
    riscv64_backend.emit_variadic_setup = generic_emit_variadic_setup;
    riscv32_backend.classify_arg = riscv32_classify_arg;
    riscv32_backend.classify_return = riscv32_classify_return;
    riscv32_backend.emit_variadic_setup = generic_emit_variadic_setup;

    ((ISARegInfo *)x86_64_backend.regs)->desc = &x86_64_backend.traits->regs;
    ((ISARegInfo *)aarch64_backend.regs)->desc = &aarch64_backend.traits->regs;
    ((ISARegInfo *)aarch32_backend.regs)->desc = &aarch32_backend.traits->regs;
    ((ISARegInfo *)riscv64_backend.regs)->desc = &riscv64_backend.traits->regs;
    ((ISARegInfo *)riscv32_backend.regs)->desc = &riscv32_backend.traits->regs;
    x86_init_asm_emission(&x86_64_backend);
    aarch64_init_asm_emission(&aarch64_backend);
    /* AArch32 asm emitters are set in aarch32_init_backend; do NOT call
     * aarch64_init_asm_emission on aarch32_backend as it would overwrite
     * the AArch32-specific asm emitters with AArch64 ones. */
    riscv64_init_asm_emission(&riscv64_backend);
    riscv64_init_asm_emission(&riscv32_backend);
    done = true;
}

const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa) {
    zero_cc_init_backends_once();
    switch (isa) {
        case ISA_X86_64: return &x86_64_backend;
        case ISA_AARCH64: return &aarch64_backend;
        case ISA_AARCH32: return &aarch32_backend;
        case ISA_RISCV64: return &riscv64_backend;
        case ISA_RISCV32: return &riscv32_backend;
        default: return NULL;
    }
}

const ZeroCCISABackend *zero_cc_get_native_backend(void) {
    zero_cc_init_backends_once();
#if defined(__x86_64__)
    return &x86_64_backend;
#elif defined(__aarch64__)
    return &aarch64_backend;
#elif defined(__riscv) && __riscv_xlen == 64
    return &riscv64_backend;
#elif defined(__riscv) && __riscv_xlen == 32
    return &riscv32_backend;
#else
    return &x86_64_backend;  /* fallback */
#endif
}
