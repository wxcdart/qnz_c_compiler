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
typedef struct ZeroCCEmitBackend   ZeroCCEmitBackend;
typedef struct ZeroCCLinkBackend   ZeroCCLinkBackend;
typedef struct ZeroCCMemoryBackend ZeroCCMemoryBackend;
typedef struct ZeroCCABICallBackend ZeroCCABICallBackend;
typedef struct ZeroCCAtomicBackend ZeroCCAtomicBackend;
typedef struct ZeroCCGenericBackend ZeroCCGenericBackend;
typedef struct ZeroCCStdlibBackend ZeroCCStdlibBackend;
typedef struct ZeroCCDiagBackend   ZeroCCDiagBackend;

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

/* Setter prototypes for the remaining backends */
void zero_cc_set_emit_backend(struct ZeroCCCompiler *c, const ZeroCCEmitBackend *b);
void zero_cc_set_link_backend(struct ZeroCCCompiler *c, const ZeroCCLinkBackend *b);
void zero_cc_set_memory_backend(struct ZeroCCCompiler *c, const ZeroCCMemoryBackend *b);
void zero_cc_set_abi_backend(struct ZeroCCCompiler *c, const ZeroCCABICallBackend *b);
void zero_cc_set_atomic_backend(struct ZeroCCCompiler *c, const ZeroCCAtomicBackend *b);
void zero_cc_set_generic_backend(struct ZeroCCCompiler *c, const ZeroCCGenericBackend *b);
void zero_cc_set_stdlib_backend(struct ZeroCCCompiler *c, const ZeroCCStdlibBackend *b);
void zero_cc_set_diag_backend(struct ZeroCCCompiler *c, const ZeroCCDiagBackend *b);

/* ----- New Backend Interfaces ------------------------------------------------ */

/* Emit Backend – abstract code‑generation primitives */
typedef struct ZeroCCEmitBackend {
    void (*emit_prologue)(struct ZeroCCCompiler *c, void *fn);
    void (*emit_epilogue)(struct ZeroCCCompiler *c, void *fn);
    void (*emit_mov)(struct ZeroCCCompiler *c, uint8_t dst, uint8_t src);
    void (*emit_alu)(struct ZeroCCCompiler *c, uint8_t op, uint8_t dst, uint8_t src);
    void (*emit_call)(struct ZeroCCCompiler *c, void *target);
    void (*emit_ret)(struct ZeroCCCompiler *c);
    void (*emit_label)(struct ZeroCCCompiler *c, const char *name);
    void (*emit_jump)(struct ZeroCCCompiler *c, const char *label);
} ZeroCCEmitBackend;

/* Link Backend – symbol resolution and relocation handling */
typedef struct ZeroCCLinkBackend {
    void *(*resolve)(struct ZeroCCCompiler *c, const char *sym);
    void (*record_reloc)(struct ZeroCCCompiler *c, const char *sym, size_t offset, uint32_t type);
} ZeroCCLinkBackend;

/* Memory Backend – allocation of executable and heap memory */
typedef struct ZeroCCMemoryBackend {
    void *(*alloc_executable)(size_t size);
    void  (*free_executable)(void *ptr, size_t size);
    void *(*alloc_heap)(size_t size);
    void  (*free_heap)(void *ptr);
} ZeroCCMemoryBackend;

/* ABI Backend – calling‑convention details */
typedef struct ZeroCCABICallBackend {
    const char **int_arg_regs;
    const char **fp_arg_regs;
    const char *int_ret;
    const char *fp_ret;
    size_t (*arg_home_offset)(size_t idx);
    const char *(*ret_reg)(struct ZeroCCType *type);
} ZeroCCABICallBackend;

/* Atomic Backend – atomic operation generation */
typedef struct ZeroCCAtomicBackend {
    void (*emit_load)(struct ZeroCCCompiler *c, const struct ZeroCCType *type, uint8_t src_reg, uint8_t *dst_reg);
    void (*emit_store)(struct ZeroCCCompiler *c, const struct ZeroCCType *type, uint8_t src_reg, uint8_t dst_reg);
    void (*emit_rmw)(struct ZeroCCCompiler *c, const struct ZeroCCType *type, uint8_t addr_reg, uint8_t val_reg, int op, uint8_t *out_reg);
    void *(*resolve_libatomic)(struct ZeroCCCompiler *c, const char *name);
} ZeroCCAtomicBackend;

/* Generic Backend – compile‑time type queries */
typedef struct ZeroCCGenericBackend {
    size_t (*sizeof_type)(const struct ZeroCCType *ty);
    size_t (*alignof_type)(const struct ZeroCCType *ty);
    void *(*runtime_generic_helper)(struct ZeroCCCompiler *c);
} ZeroCCGenericBackend;

/* Stdlib Backend – virtual header / builtin macro provider */
typedef struct ZeroCCStdlibBackend {
    const char *(*virtual_header)(const char *name);
    const char *(*builtin_macro)(const char *name);
} ZeroCCStdlibBackend;

/* Diagnostics Backend – error / warning reporting */
typedef struct ZeroCCDiagBackend {
    void (*error)(struct ZeroCCCompiler *c, const char *msg, size_t line, size_t col);
    void (*warning)(struct ZeroCCCompiler *c, const char *msg, size_t line, size_t col);
    void (*note)(struct ZeroCCCompiler *c, const char *msg, size_t line, size_t col);
} ZeroCCDiagBackend;

/* Setter prototypes are declared earlier; implementations live in backends/setters.c */
void zero_cc_init_backends(struct ZeroCCCompiler *c);

#endif /* ZERO_CC_BACKENDS_H */