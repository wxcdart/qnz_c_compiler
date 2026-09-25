#include "zero_cc/zero_cc_backends.h"
#include "zero_cc/types_lexer.h"
#include <stdio.h>

void zero_cc_set_tls_backend_impl(ZeroCCCompiler *c, const void *b) {
    extern const ZeroCCTLSBackend zero_cc_default_tls_backend;
    c->tls_backend = b ? (const ZeroCCTLSBackend*)b : &zero_cc_default_tls_backend;
}
