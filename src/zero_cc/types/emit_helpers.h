#ifndef ZERO_CC_TYPES_EMIT_HELPERS_H
#define ZERO_CC_TYPES_EMIT_HELPERS_H

/* Lexer implementation (moved to lexer.c/lexer.h) */
void zero_cc_tokenize_c99(const char *p, ZeroCCCompiler *c);
ZeroCCTokType zero_cc_keyword_lookup(const char *s, size_t len);
/* Machine Code Emission Helpers (x86_64) */
static inline void zero_cc_emit_u8(ZeroCCBuffer *b, uint8_t byte) {
    zero_cc_buf_put_bytes(b, &byte, 1);
}
static inline void zero_cc_emit_u32(ZeroCCBuffer *b, uint32_t val) {
    zero_cc_buf_put_bytes(b, &val, 4);
}
static inline void zero_cc_emit_u64(ZeroCCBuffer *b, uint64_t val) {
    zero_cc_buf_put_bytes(b, &val, 8);
}

/* Abstraction layer: emit helpers that dispatch by target internally */
static inline void zero_cc_emit_prologue(ZeroCCCompiler *c, uint32_t stack_alloc) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)stack_alloc; /* TODO: implement ARM64 encoding */
#else
    zero_cc_emit_u8(&c->text_section, 0x55);
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x89);
    zero_cc_emit_u8(&c->text_section, 0xE5);
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x81);
    zero_cc_emit_u8(&c->text_section, 0xEC);
    zero_cc_emit_u32(&c->text_section, stack_alloc);
#endif
}

static inline void zero_cc_emit_epilogue(ZeroCCCompiler *c) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; /* TODO: implement ARM64 epilogue */
#else
    zero_cc_emit_u8(&c->text_section, 0xC9);
    zero_cc_emit_u8(&c->text_section, 0xC3);
#endif
}

static inline void zero_cc_emit_mov_imm64(ZeroCCCompiler *c, int reg_idx, uint64_t imm) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)reg_idx; (void)imm; /* TODO: implement ARM64 mov immediate sequences */
#else
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8 + (uint8_t)reg_idx);
    zero_cc_emit_u64(&c->text_section, imm);
#endif
}

static inline void zero_cc_emit_movss_rodata(ZeroCCCompiler *c, int xmm_idx, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)xmm_idx; (void)rodata_offset; /* TODO: implement AArch64 single-precision literal load (ldr s0, ...) */
#else
    /* movss xmmN, [rip + disp32] -> F3 0F 10 05 disp32 */
    (void)xmm_idx;
    zero_cc_emit_u8(&c->text_section, 0xF3);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x10);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}

static inline void zero_cc_emit_movsd_rodata(ZeroCCCompiler *c, int xmm_idx, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)xmm_idx; (void)rodata_offset; /* TODO: implement AArch64 double-precision literal load (ldr d0, ...) */
#else
    /* movsd xmmN, [rip + disp32] -> F2 0F 10 05 disp32 */
    (void)xmm_idx;
    zero_cc_emit_u8(&c->text_section, 0xF2);
    zero_cc_emit_u8(&c->text_section, 0x0F);
    zero_cc_emit_u8(&c->text_section, 0x10);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}

static inline void zero_cc_emit_lea_rax_rodata(ZeroCCCompiler *c, size_t rodata_offset) {
#if defined(ZERO_CC_TARGET_ARM64)
    (void)c; (void)rodata_offset; /* TODO: implement ADRP/ADD literal sequence for ARM64 */
#else
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0x8D);
    zero_cc_emit_u8(&c->text_section, 0x05);
    size_t patch_pos = c->text_section.length;
    zero_cc_emit_u32(&c->text_section, 0);
    if (c->str_reloc_count < ZERO_CC_MAX_STR_RELOCS) {
        c->str_relocs[c->str_reloc_count].text_patch_offset = patch_pos;
        c->str_relocs[c->str_reloc_count].rodata_offset = rodata_offset;
        c->str_reloc_count++;
    }
#endif
}


#endif /* ZERO_CC_TYPES_EMIT_HELPERS_H */
