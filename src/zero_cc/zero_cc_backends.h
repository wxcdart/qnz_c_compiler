/* zero_cc_backends.h – Backend interface definitions for zero_cc
   Currently only TLS backend is defined. Additional backends (codegen, symbol, etc.)
   can be added in the future following the same pattern. */
#ifndef ZERO_CC_BACKENDS_H
#define ZERO_CC_BACKENDS_H

#include "types_lexer.h"

/* TLS Backend – handles _Thread_local storage */
typedef struct ZeroCCTLSBackend {
    /* Allocate/register a thread‑local variable.
       Returns an opaque handle that will be passed to the emit callbacks. */
    void *(*declare)(ZeroCCCompiler *c,
                     const char *name,
                     const ZeroCCType *type,
                     int64_t init_value);

    /* Emit code that loads the TLS variable into RAX. */
    void (*emit_load)(ZeroCCCompiler *c, const void *handle);

    /* Emit code that stores RAX into the TLS variable. */
    void (*emit_store)(ZeroCCCompiler *c, const void *handle);

    /* Emit code that obtains the address of the TLS variable (for &ident). */
    void (*emit_address)(ZeroCCCompiler *c, const void *handle);

    /* Optional finalisation hook (e.g., free resources). */
    void (*finalize)(ZeroCCCompiler *c);
} ZeroCCTLSBackend;

/* Setter – NULL resets to built‑in default static‑alias backend. */
static inline void zero_cc_set_tls_backend(ZeroCCCompiler *c,
                                           const ZeroCCTLSBackend *b)
{
    extern const ZeroCCTLSBackend zero_cc_default_tls_backend; /* defined in tls_backend_static.c */
    c->tls_backend = b ? b : &zero_cc_default_tls_backend;
}

#endif /* ZERO_CC_BACKENDS_H */
