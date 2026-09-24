/* =========================================================================
 * zero_arm64_emitter.h - AArch64 / ARM64 Emitter Helpers
 * Part of zero_cc/
 * ========================================================================= */
#ifndef ZERO_ARM64_EMITTER_H
#define ZERO_ARM64_EMITTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Part 49: AArch64 / ARM64 Emitter Helpers & JIT DWARF Debug Symbols
 * ========================================================================= */

typedef struct ZeroArm64Code {
    uint32_t *instructions;
    size_t count;
    size_t capacity;
} ZeroArm64Code;

static inline void zero_arm64_emit(ZeroArm64Code *code, uint32_t inst) {
    if (code && code->count < code->capacity) {
        code->instructions[code->count++] = inst;
    }
}

/* Emits: ADD W0, W0, W1 (0x0B010000) */
static inline void zero_arm64_add_w0_w0_w1(ZeroArm64Code *code) {
    zero_arm64_emit(code, 0x0B010000);
}

/* Emits: RET (0xD65F03C0) */
static inline void zero_arm64_ret(ZeroArm64Code *code) {
    zero_arm64_emit(code, 0xD65F03C0);
}

#ifdef __cplusplus
}
#endif

#endif /* ZERO_ARM64_EMITTER_H */
