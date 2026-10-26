#ifndef ZERO_CC_TYPES_EMIT_HELPERS_H
#define ZERO_CC_TYPES_EMIT_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include "zero_cc/core/buffers.h"

/* Lexer implementation (moved to lexer.c/lexer.h) */
/* Functions now static inline in lexer headers */
/* void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c); */
/* ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len); */
/* Machine Code Emission Helpers (x86_64)
 *
 * The u8/u16/u32/u64 primitives are the only generic byte emitters left:
 * ISA backends (src/zero_cc/isa) build their own instruction encodings on
 * top of them. Higher-level stubs that used to live here (prologue/epilogue,
 * mov-imm64, movss/movsd rodata loads, lea-rax-rodata) were superseded by
 * the IR/machine-emitter pipeline and removed in Sprint 56. */
static inline void zero_cc_emit_u8(ZeroCCBuffer *b, uint8_t byte) {
    zero_cc_buf_put_bytes(b, &byte, 1);
}
static inline void zero_cc_emit_u16(ZeroCCBuffer *b, uint16_t val) {
    zero_cc_buf_put_bytes(b, &val, 2);
}
static inline void zero_cc_emit_u32(ZeroCCBuffer *b, uint32_t val) {
    zero_cc_buf_put_bytes(b, &val, 4);
}
static inline void zero_cc_emit_u64(ZeroCCBuffer *b, uint64_t val) {
    zero_cc_buf_put_bytes(b, &val, 8);
}

#endif /* ZERO_CC_TYPES_EMIT_HELPERS_H */
