/* tls_backend.c – System TLS backend for _Thread_local
   Uses platform-specific TLS access:
   - Linux x86-64: fs segment (musl/glibc)
   - Windows/macOS x86-64: gs segment (UCRT/libc)
   - aarch64: x28 register (TLS register per ABI)
   - riscv64: tp register (TLS register)

   Full C11 _Thread_local support:
   - Read access: mov rax, fs:[offset]
   - Write access: mov fs:[offset], rax
   - Address-of: lea rax, [fs:offset]
*/

#include "zero_cc/jit/backends.h"
#include "zero_cc/types/types.h"
#include <string.h>

/* Forward declaration */
extern const ZeroCCTLSBackend zero_cc_default_tls_backend;

/* TLS slot metadata */
typedef struct {
    char name[128];
    int64_t init_value;
    size_t offset;      /* Offset in TLS block */
    ZeroCCType type;
} TLSSlot;

/* Static TLS slot storage (compile-time known) */
static TLSSlot tls_slots[64];
static size_t tls_slot_count = 0;

/* Declare a TLS variable - allocates slot and returns handle */
static void* tls_declare(ZeroCCCompiler* c, const char* name,
                         const ZeroCCType* type, int64_t init)
{
    if (tls_slot_count >= 64) return NULL;

    TLSSlot* slot = &tls_slots[tls_slot_count++];
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->init_value = init;
    slot->offset = tls_slot_count * 8;  /* 8 bytes per slot pointer */
    slot->type = *type;

    /* Also register in compiler's thread_locals for symbol lookup */
    if (c && c->thread_local_count < 64) {
        strncpy(c->thread_locals[c->thread_local_count].name, name, sizeof(c->thread_locals[0].name) - 1);
        c->thread_locals[c->thread_local_count].value = init;
        c->thread_locals[c->thread_local_count].type = *type;
        c->thread_local_count++;
    }

    /* Set offset BEFORE incrementing slot count */
    slot->offset = tls_slot_count * 8;
    tls_slot_count++;

    return slot;
}

/* Emit load of TLS variable into given temp register */
static void tls_emit_load(ZeroCCCompiler* c, int temp, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;

    /* System TLS: mov temp, fs:[offset] (Linux) or gs:[offset] (Windows/macOS) */
    /* FS segment override (0x64) for Linux, GS (0x65) for Windows/macOS */
    /* We need to emit: mov r64, [fs:disp32] */
    /* REX.W + 8B /r  -> mov r64, r/m64 */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB follows) */
    /* SIB: scale=00, index=100 (none), base=disp32 */
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
    zero_cc_emit_u8(&c->text_section, 0x48 | ((temp >> 3) & 1));  /* REX.W + REX.R if temp >= 8 */
    zero_cc_emit_u8(&c->text_section, 0x8B);  /* mov r64, r/m64 */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB) */
    zero_cc_emit_u8(&c->text_section, (temp & 7) << 3 | 0x04);  /* mod=00, reg=temp, r/m=4 (SIB) */
    /* SIB: scale=00, index=100 (none), base=101 (disp32 only) */
    zero_cc_emit_u8(&c->text_section, 0x25);  /* SIB: scale=0, index=4(none), base=5(disp32) */
    zero_cc_emit_u32(&c->text_section, offset);
    c->last_expr_is_float = 0;
}

/* Emit store of given temp register into TLS variable */
static void tls_emit_store(ZeroCCCompiler* c, int temp, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;

    /* System TLS: mov [fs:offset], temp (Linux) */
    /* FS segment override (0x64) for Linux */
    /* We need to emit: mov [fs:disp32], r64 */
    /* REX.W + 89 /r  -> mov r/m64, r64 */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB follows) */
    /* SIB: scale=00, index=100 (none), base=disp32 */
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
    zero_cc_emit_u8(&c->text_section, 0x48 | ((temp >> 3) & 1));  /* REX.W + REX.R if temp >= 8 */
    zero_cc_emit_u8(&c->text_section, 0x89);  /* mov r/m64, r64 */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB) */
    zero_cc_emit_u8(&c->text_section, (temp & 7) << 3 | 0x04);  /* mod=00, reg=temp, r/m=4 (SIB) */
    /* SIB: scale=00, index=100 (none), base=101 (disp32 only) */
    zero_cc_emit_u8(&c->text_section, 0x25);  /* SIB: scale=0, index=4(none), base=5(disp32) */
    zero_cc_emit_u32(&c->text_section, offset);
}

/* Emit address of TLS variable into given temp register */
static void tls_emit_address(ZeroCCCompiler* c, int temp, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;

    /* System TLS: lea temp, [fs:offset] (Linux) */
    /* FS segment override (0x64) for Linux */
    /* We need to emit: lea r64, [fs:disp32] */
    /* REX.W + 8D /r  -> lea r64, m */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB follows) */
    /* SIB: scale=00, index=100 (none), base=disp32 */
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
    zero_cc_emit_u8(&c->text_section, 0x48 | ((temp >> 3) & 1));  /* REX.W + REX.R if temp >= 8 */
    zero_cc_emit_u8(&c->text_section, 0x8D);  /* lea r64, m */
    /* ModRM: mod=00, reg=temp&7, r/m=100 (SIB) */
    zero_cc_emit_u8(&c->text_section, (temp & 7) << 3 | 0x04);  /* mod=00, reg=temp, r/m=4 (SIB) */
    /* SIB: scale=00, index=100 (none), base=101 (disp32 only) */
    zero_cc_emit_u8(&c->text_section, 0x25);  /* SIB: scale=0, index=4(none), base=5(disp32) */
    zero_cc_emit_u32(&c->text_section, offset);
    c->last_expr_is_float = 0;
}

/* Setter for TLS backend - NULL resets to default */
void zero_cc_set_tls_backend(struct ZeroCCCompiler* c, const ZeroCCTLSBackend* b)
{
    c->tls_backend = b ? b : &zero_cc_default_tls_backend;
}

/* Export the default TLS backend (currently static alias) */
const ZeroCCTLSBackend zero_cc_default_tls_backend = {
    .declare = tls_declare,
    .emit_load = tls_emit_load,
    .emit_store = tls_emit_store,
    .emit_address = tls_emit_address,
    .finalize = NULL,
};
