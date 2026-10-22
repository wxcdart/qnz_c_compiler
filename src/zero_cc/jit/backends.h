/* zero_cc_backends.h – Backend interface definitions for zero_cc */
#ifndef ZERO_CC_BACKENDS_H
#define ZERO_CC_BACKENDS_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations for core types – only opaque pointers are needed here. */
struct ZeroCCCompiler;
struct ZeroCCType;
/* Forward declarations for backend types – needed because zero_cc_backends.h is
   included from types_lexer.h before the actual struct definitions appear. */
typedef struct ZeroCCTLSBackend ZeroCCTLSBackend;

/* TLS Backend – handles _Thread_local storage */
typedef struct ZeroCCTLSBackend {
    /* Allocate/register a thread‑local variable.
       Returns an opaque handle that will be passed to the emit callbacks. */
    void *(*declare)(struct ZeroCCCompiler *c,
                     const char *name,
                     const struct ZeroCCType *type,
                     int64_t init_value);

    /* Emit code that loads the TLS variable into RAX. */
    void (*emit_load)(struct ZeroCCCompiler *c, const void *handle);

    /* Emit code that stores RAX into the TLS variable. */
    void (*emit_store)(struct ZeroCCCompiler *c, const void *handle);

    /* Emit code that obtains the address of the TLS variable (for &ident). */
    void (*emit_address)(struct ZeroCCCompiler *c, const void *handle);

    /* Optional finalisation hook (e.g., free resources). */
    void (*finalize)(struct ZeroCCCompiler *c);
} ZeroCCTLSBackend;

/* Setter – NULL resets to built‑in default static‑alias backend. */
void zero_cc_set_tls_backend(struct ZeroCCCompiler *c,
                                 const ZeroCCTLSBackend *b);





#endif /* ZERO_CC_BACKENDS_H */
