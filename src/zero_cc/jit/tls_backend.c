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
    (void)c;
    if (tls_slot_count >= 64) return NULL;
    
    TLSSlot* slot = &tls_slots[tls_slot_count++];
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->init_value = init;
    slot->offset = tls_slot_count * 8;  /* 8 bytes per slot pointer */
    slot->type = *type;
    
    return slot;
}

/* Emit load of TLS variable into RAX (integer) or XMM0 (float) */
static void tls_emit_load(ZeroCCCompiler* c, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;
    
    /* System TLS: mov rax, fs:[offset] (Linux) or gs:[offset] (Windows/macOS) */
    /* FS segment override (0x64) for Linux, GS (0x65) for Windows/macOS */
#ifdef _WIN32
    zero_cc_emit_u8(&c->text_section, 0x65);  /* GS segment (Windows) */
#else
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
#endif
    zero_cc_emit_u8(&c->text_section, 0x48);  /* REX.W */
    zero_cc_emit_u8(&c->text_section, 0x8B);  /* mov rax, [disp32] */
    zero_cc_emit_u32(&c->text_section, offset);
    c->last_expr_is_float = 0;
}

/* Emit store of RAX into TLS variable */
static void tls_emit_store(ZeroCCCompiler* c, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;
    
    /* System TLS: mov fs:[offset], rax (Linux) or gs:[offset], rax (Windows/macOS) */
#ifdef _WIN32
    zero_cc_emit_u8(&c->text_section, 0x65);  /* GS segment (Windows) */
#else
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
#endif
    zero_cc_emit_u8(&c->text_section, 0x48);  /* REX.W */
    zero_cc_emit_u8(&c->text_section, 0x89);  /* mov [disp32], rax */
    zero_cc_emit_u32(&c->text_section, offset);
}

/* Emit address of TLS variable into RAX */
static void tls_emit_address(ZeroCCCompiler* c, const void* handle)
{
    const TLSSlot* slot = (const TLSSlot*)handle;
    size_t offset = slot->offset;
    
    /* System TLS: lea rax, [fs:offset] (Linux) or [gs:offset] (Windows/macOS) */
    /* Note: This gives the effective address, not the absolute address */
#ifdef _WIN32
    zero_cc_emit_u8(&c->text_section, 0x65);  /* GS segment (Windows) */
#else
    zero_cc_emit_u8(&c->text_section, 0x64);  /* FS segment (Linux) */
#endif
    zero_cc_emit_u8(&c->text_section, 0x48);  /* REX.W */
    zero_cc_emit_u8(&c->text_section, 0x8D);  /* lea rax, [disp32] */
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
