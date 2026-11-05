/* ISA backend registry (Sprint 114).
 *
 * Central lookup from a ZeroCCISA target to its ZeroCCISABackend vtable.
 * Lives in its own translation unit (extracted from x86_64.c) so that no
 * single ISA emitter owns the global registry - adding a target means
 * adding its vtable + one case here, without touching another ISA file.
 */
#include <stdbool.h>
#include "zero_cc/isa/isa_backend.h"

/* Fill in ASM-emission function pointers once. The backend tables are
 * writable statics (not .rodata), so no const cast is needed; callers only
 * ever see them through const pointers. */
static void zero_cc_init_backends_once(void) {
    static bool done = false;
    if (done) return;
    riscv32_init_backend();  /* copy RV64 table + override RV32 fields (riscv32.c) */
    x86_init_asm_emission(&x86_64_backend);
    aarch64_init_asm_emission(&aarch64_backend);
    riscv64_init_asm_emission(&riscv64_backend);
    riscv64_init_asm_emission(&riscv32_backend);
    done = true;
}

const ZeroCCISABackend *zero_cc_get_backend(ZeroCCISA isa) {
    zero_cc_init_backends_once();
    switch (isa) {
        case ISA_X86_64: return &x86_64_backend;
        case ISA_AARCH64: return &aarch64_backend;
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
