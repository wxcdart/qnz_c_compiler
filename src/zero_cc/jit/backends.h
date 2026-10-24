/* zero_cc_backends.h – Backend interface definitions for zero_cc */
#ifndef ZERO_CC_BACKENDS_H
#define ZERO_CC_BACKENDS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Forward declarations for core types – only opaque pointers are needed here. */
struct ZeroCCCompiler;
struct ZeroCCType;
/* Forward declarations for backend types – needed because zero_cc_backends.h is
   included from types_lexer.h before the actual struct definitions appear. */
typedef struct ZeroCCTLSBackend ZeroCCTLSBackend;

/* TLS Backend – handles _Thread_local storage (static TLS model).
 *
 * `declare` allocates storage for a program-local TLS variable and returns
 * its byte offset within the program's TLS block, or -1 on error.  In JIT
 * mode the block is the data section (the variable is a normal global);
 * for AOT x86-64 the block is .tdata+.tbss and `finalize` (called after
 * all top-level declarations) packs the sections and patches each
 * variable's registered data_offset.  Codegen accesses the variable's
 * address via OP_TLS_LEA (AOT: R_X86_64_TPOFF64) or OP_MOV_GLOBAL (JIT). */
typedef struct ZeroCCTLSBackend {
    int (*declare)(struct ZeroCCCompiler *c,
                   const char *name,
                   const struct ZeroCCType *type,
                   int64_t init_value,
                   bool has_init);

    /* Lay out the TLS block (.tdata then .tbss) and finalize per-variable
     * offsets. Called once, after all top-level declarations are parsed. */
    void (*finalize)(struct ZeroCCCompiler *c);
} ZeroCCTLSBackend;

/* Setter – NULL resets to built‑in default static‑alias backend. */
void zero_cc_set_tls_backend(struct ZeroCCCompiler *c,
                                 const ZeroCCTLSBackend *b);





#endif /* ZERO_CC_BACKENDS_H */
