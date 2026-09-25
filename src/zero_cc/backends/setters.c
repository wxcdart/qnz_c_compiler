/* -----------------------------------------------------------------
 *  setters.c – implements the backend setters (needs the full compiler)
 * ----------------------------------------------------------------- */
#include "zero_cc/zero_cc_backends.h"
#include "zero_cc/types_lexer.h"
#include <stdio.h>
#include <stdlib.h>

/* TLS */
void zero_cc_set_tls_backend(struct ZeroCCCompiler *c,
                             const ZeroCCTLSBackend *b)
{
    extern const ZeroCCTLSBackend zero_cc_default_tls_backend;
    c->tls_backend = b ? b : &zero_cc_default_tls_backend;
}

/* The remaining setters abort on a NULL argument – exactly the policy
 * the project wants (no graceful fallback). */
#define SETTER_IMPL(name, type)                                 \
void zero_cc_set_##name(struct ZeroCCCompiler *c,               \
                        const type *b)                         \
{                                                               \
    if (!b) {                                                   \
        fprintf(stderr, "zero_cc_set_" #name ": NULL backend – aborting\\n"); \
        abort();                                                \
    }                                                           \
    c->name##_backend = b;                                      \
}

SETTER_IMPL(emit,    ZeroCCEmitBackend)
SETTER_IMPL(link,    ZeroCCLinkBackend)
SETTER_IMPL(memory,  ZeroCCMemoryBackend)
SETTER_IMPL(abi,     ZeroCCABICallBackend)
SETTER_IMPL(atomic,  ZeroCCAtomicBackend)
SETTER_IMPL(generic, ZeroCCGenericBackend)
SETTER_IMPL(stdlib,  ZeroCCStdlibBackend)
SETTER_IMPL(diag,    ZeroCCDiagBackend)

#undef SETTER_IMPL