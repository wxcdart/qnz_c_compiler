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
    aarch64_backend.classify_arg = aarch64_classify_arg;
    aarch64_backend.classify_return = aarch64_classify_return;
    aarch32_backend.classify_arg = aarch32_classify_arg;
    aarch32_backend.classify_return = aarch32_classify_return;
    riscv64_backend.classify_arg = riscv64_classify_arg;
    riscv64_backend.classify_return = riscv64_classify_return;
    riscv32_backend.classify_arg = riscv32_classify_arg;
    riscv32_backend.classify_return = riscv32_classify_return;

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
