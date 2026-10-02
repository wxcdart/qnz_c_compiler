/* Machine Code Emitter - Wrappers for x86-64 code generation */
#ifndef ZERO_CC_PARSER_MACHINE_EMITTER_H
#define ZERO_CC_PARSER_MACHINE_EMITTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "zero_cc/types/types.h"

/* Forward declarations */
typedef struct ZeroCCCompiler ZeroCCCompiler;

/* Emit a single byte */
static inline void me_emit_u8(ZeroCCCompiler *c, uint8_t byte) {
    zero_cc_emit_u8(&c->text_section, byte);
}

/* Emit 16-bit value */
static inline void me_emit_u16(ZeroCCCompiler *c, uint16_t val) {
    zero_cc_emit_u16(&c->text_section, val);
}

/* Emit 32-bit value */
static inline void me_emit_u32(ZeroCCCompiler *c, uint32_t val) {
    zero_cc_emit_u32(&c->text_section, val);
}

/* Emit 64-bit value */
static inline void me_emit_u64(ZeroCCCompiler *c, uint64_t val) {
    zero_cc_emit_u64(&c->text_section, val);
}

/* Emit bytes */
static inline void me_emit_bytes(ZeroCCCompiler *c, const uint8_t *bytes, size_t len) {
    zero_cc_buf_put_bytes(&c->text_section, bytes, len);
}

/* Get current code offset */
static inline size_t me_get_offset(ZeroCCCompiler *c) {
    return c->text_section.length;
}

/* Patch offset at position */
static inline void me_patch_u32(ZeroCCCompiler *c, size_t pos, uint32_t val) {
    memcpy(c->text_section.data + pos, &val, 4);
}

/* Emit MOV rax, imm64 */
static inline void me_mov_rax_imm(ZeroCCCompiler *c, uint64_t imm) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0xB8);
    me_emit_u64(c, imm);
}

/* Emit MOV [rbp - offset], rax */
static inline void me_mov_mem_rax(ZeroCCCompiler *c, int32_t offset) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0x85);
    me_emit_u32(c, (uint32_t)(-offset));
}

/* Emit MOV rax, [rbp - offset] */
static inline void me_mov_rax_mem(ZeroCCCompiler *c, int32_t offset) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x8B);
    me_emit_u8(c, 0x85);
    me_emit_u32(c, (uint32_t)(-offset));
}

/* Emit LEA rax, [rbp - offset] */
static inline void me_lea_rax_mem(ZeroCCCompiler *c, int32_t offset) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x8D);
    me_emit_u8(c, 0x85);
    me_emit_u32(c, (uint32_t)(-offset));
}

/* Emit JMP rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jmp_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xE9);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JE rel32 (forward reference, returns patch position) */
static inline size_t me_emit_je_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x84);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JNE rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jne_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x85);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JL rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jl_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8C);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JLE rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jle_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8E);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JG rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jg_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8F);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit JGE rel32 (forward reference, returns patch position) */
static inline size_t me_emit_jge_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0x8D);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Patch relative jump at position */
static inline void me_patch_jmp_rel32(ZeroCCCompiler *c, size_t patch_pos, size_t target) {
    int32_t rel = (int32_t)(target - (patch_pos + 4));
    me_patch_u32(c, patch_pos, (uint32_t)rel);
}

/* Emit CALL rel32 */
static inline size_t me_emit_call_rel32(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xE8);
    size_t pos = me_get_offset(c);
    me_emit_u32(c, 0);  /* Placeholder */
    return pos;
}

/* Emit RET */
static inline void me_emit_ret(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xC3);
}

/* Emit LEAVE */
static inline void me_emit_leave(ZeroCCCompiler *c) {
    me_emit_u8(c, 0xC9);
}

/* Emit PUSH rax */
static inline void me_push_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x50);
}

/* Emit POP rax */
static inline void me_pop_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x58);
}

/* Emit PUSH rcx */
static inline void me_push_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x51);
}

/* Emit POP rcx */
static inline void me_pop_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x59);
}

/* Emit PUSH rbx */
static inline void me_push_rbx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x53);
}

/* Emit POP rbx */
static inline void me_pop_rbx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x5B);
}

/* Emit ADD rax, rcx */
static inline void me_add_rax_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x01);
    me_emit_u8(c, 0xC8);
}

/* Emit SUB rcx, rax */
static inline void me_sub_rcx_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x29);
    me_emit_u8(c, 0xC1);
}

/* Emit MOV rax, rcx */
static inline void me_mov_rax_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xC8);
}

/* Emit MOV rbx, rax */
static inline void me_mov_rbx_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xC3);
}

/* Emit MOV rax, rdx */
static inline void me_mov_rax_rdx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xD0);
}

/* Emit IMUL rax, rcx */
static inline void me_imul_rax_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x0F);
    me_emit_u8(c, 0xAF);
    me_emit_u8(c, 0xC1);
}

/* Emit CQO (sign extend rax into rdx:rax) */
static inline void me_cqo(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x99);
}

/* Emit IDIV rbx */
static inline void me_idiv_rbx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0xF7);
    me_emit_u8(c, 0xFB);
}

/* Emit TEST rax, rax */
static inline void me_test_rax_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x85);
    me_emit_u8(c, 0xC0);
}

/* Emit CMP rax, rcx */
static inline void me_cmp_rax_rcx(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x39);
    me_emit_u8(c, 0xC8);
}

/* Emit CMP rcx, rax */
static inline void me_cmp_rcx_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x39);
    me_emit_u8(c, 0xC1);
}

/* Emit XOR rax, rax */
static inline void me_xor_rax_rax(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x31);
    me_emit_u8(c, 0xC0);
}

/* Emit prologue */
static inline void me_emit_prologue(ZeroCCCompiler *c) {
    me_emit_u8(c, 0x55);          /* push rbp */
    me_emit_u8(c, 0x48);          /* mov rbp, rsp */
    me_emit_u8(c, 0x89);
    me_emit_u8(c, 0xE5);
}

/* Emit stack allocation: sub rsp, size */
static inline void me_emit_stack_alloc(ZeroCCCompiler *c, uint32_t size) {
    me_emit_u8(c, 0x48);
    me_emit_u8(c, 0x81);
    me_emit_u8(c, 0xEC);
    me_emit_u32(c, size);
}

#endif /* ZERO_CC_PARSER_MACHINE_EMITTER_H */
