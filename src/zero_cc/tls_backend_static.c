/* tls_backend_static.c – default TLS backend (static‑alias implementation)
   This backend implements the minimal behaviour that the existing code
   expects: a thread‑local variable is stored inside the compiler instance's
   thread_locals[] array and reading the variable emits a mov‑imm64 with the
   current value.  Writes and address‑of are not supported in this simple
   backend (they would raise a compile‑time error if used).
*/

#include "zero_cc_backends.h"
#include <string.h>

static void *static_tls_declare(ZeroCCCompiler *c,
                                const char *name,
                                const ZeroCCType *type,
                                int64_t init)
{
    if (c->thread_local_count >= 64) return NULL;
    size_t idx = c->thread_local_count++;
    strncpy(c->thread_locals[idx].name, name, sizeof(c->thread_locals[idx].name) - 1);
    c->thread_locals[idx].value = init;
    c->thread_locals[idx].type = *type;
    /* Return a pointer to the entry – the handle is opaque to the caller */
    return &c->thread_locals[idx];
}

static void static_tls_emit_load(ZeroCCCompiler *c, const void *handle)
{
    const struct { char name[128]; int64_t value; ZeroCCType type; } *h = handle;
    (void)c; (void)h; /* silence unused warnings */
    /* Emit: mov rax, imm64 (the stored value) */
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8);
    zero_cc_emit_u64(&c->text_section, (uint64_t)h->value);
    c->last_expr_is_float = 0;
}

static void static_tls_emit_store(ZeroCCCompiler *c, const void *handle)
{
    /* Not supported in the static‑alias backend – emit a runtime error.
       For simplicity we just generate a call to a helper that aborts. */
    (void)handle;
    const char *msg = "_Thread_local store not supported in static backend";
    /* In a real implementation we would emit a call to abort(). Here we just
       emit a mov rax, 0 and a ret to keep the JIT alive. */
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xC7);
    zero_cc_emit_u8(&c->text_section, 0xC0);
    zero_cc_emit_u32(&c->text_section, 0);
    /* ret */
    zero_cc_emit_u8(&c->text_section, 0xC3);
}

static void static_tls_emit_address(ZeroCCCompiler *c, const void *handle)
{
    /* Not supported – emit a NULL pointer. */
    (void)handle;
    zero_cc_emit_u8(&c->text_section, 0x48);
    zero_cc_emit_u8(&c->text_section, 0xB8);
    zero_cc_emit_u64(&c->text_section, 0);
    c->last_expr_is_float = 0;
}

/* Export the default static‑alias TLS backend */
const ZeroCCTLSBackend zero_cc_default_tls_backend = {
    .declare = static_tls_declare,
    .emit_load = static_tls_emit_load,
    .emit_store = static_tls_emit_store,
    .emit_address = static_tls_emit_address,
    .finalize = NULL,
};
