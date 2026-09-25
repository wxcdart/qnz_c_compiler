#include "zero_cc_backends.h"
#include "core.h"   /* for zero_cc_emit_u8, zero_cc_emit_u64, etc. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <dlfcn.h>

/* Minimal emit backend – forwards to existing byte emit helpers where possible */
static void emit_raw_u8(ZeroCCCompiler *c, uint8_t byte) {
    zero_cc_emit_u8(&c->text_section, byte);
}
static void emit_raw_u64(ZeroCCCompiler *c, uint64_t val) {
    zero_cc_emit_u64(&c->text_section, val);
}
static const ZeroCCEmitBackend minimal_emit = {
    .emit_prologue = NULL,
    .emit_epilogue = NULL,
    .emit_mov      = NULL,
    .emit_alu      = NULL,
    .emit_call     = NULL,
    .emit_ret      = NULL,
    .emit_label    = NULL,
    .emit_jump     = NULL,
};

/* Minimal link backend – uses the existing dlsym fallback */
static void *link_resolve_fwd(ZeroCCCompiler *c, const char *sym) {
    (void)c; return zero_cc_resolve_symbol(sym);
}
static void link_record_fwd(ZeroCCCompiler *c, const char *sym, size_t off, uint32_t type) {
    if (c->reloc_count < ZERO_CC_MAX_RELOCS) {
        ZeroCCReloc *r = &c->relocs[c->reloc_count++];
        r->text_patch_offset = off;
        snprintf(r->target_symbol, sizeof(r->target_symbol), "%s", sym);
        r->is_call = (type != 0);
    }
}
static const ZeroCCLinkBackend minimal_link = {
    .resolve = link_resolve_fwd,
    .record_reloc = link_record_fwd,
};

/* Minimal memory backend – mmap for exec, malloc for heap */
static void *mem_alloc_exe(size_t sz) {
    void *p = mmap(NULL, sz, PROT_READ|PROT_WRITE|PROT_EXEC,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void mem_free_exe(void *p, size_t sz) { munmap(p, sz); }
static void *mem_alloc_heap(size_t sz) { return malloc(sz); }
static void mem_free_heap(void *p) { free(p); }
static const ZeroCCMemoryBackend minimal_memory = {
    .alloc_executable = mem_alloc_exe,
    .free_executable  = mem_free_exe,
    .alloc_heap       = mem_alloc_heap,
    .free_heap        = mem_free_heap,
};

/* Minimal ABI backend – use the existing regmap */
static const ZeroCCABICallBackend minimal_abi = {
    .int_arg_regs = NULL, .fp_arg_regs = NULL, .int_ret = NULL, .fp_ret = NULL,
    .arg_home_offset = NULL, .ret_reg = NULL,
};

/* Minimal atomic backend – only resolves libatomic symbols */
static void *atomic_resolve_fwd(ZeroCCCompiler *c, const char *name) {
    (void)c; return zero_cc_resolve_symbol(name);
}
static const ZeroCCAtomicBackend minimal_atomic = {
    .emit_load = NULL,
    .emit_store = NULL,
    .emit_rmw = NULL,
    .resolve_libatomic = atomic_resolve_fwd,
};

/* Minimal generic backend – just return size/align from the type */
static size_t generic_sizeof_fwd(const ZeroCCType *t) { return t->size; }
static size_t generic_alignof_fwd(const ZeroCCType *t){ return t->align; }
static const ZeroCCGenericBackend minimal_generic = {
    .sizeof_type = generic_sizeof_fwd,
    .alignof_type = generic_alignof_fwd,
    .runtime_generic_helper = NULL,
};

/* Minimal stdlib backend – re‑use the existing virtual‑header function */
static const ZeroCCStdlibBackend minimal_stdlib = {
    .virtual_header = zero_cc_cpp_get_virtual_header,
    .builtin_macro  = NULL,
};

/* Minimal diagnostic backend – write to compiler fields */
static void diag_error_fwd(ZeroCCCompiler *c, const char *msg, size_t line, size_t col) {
    c->had_error = 1;
    snprintf(c->error_msg, sizeof(c->error_msg), "%s (line %zu col %zu)", msg, line, col);
}
static const ZeroCCDiagBackend minimal_diag = {
    .error = diag_error_fwd,
    .warning = NULL,
    .note = NULL,
};

/* Public initializer – registers the minimal backends */
void zero_cc_init_backends(ZeroCCCompiler *c) {
    /* The compiler pointer is unused by our minimal setters, but we keep the API */
    zero_cc_set_emit_backend   (c, &minimal_emit);
    zero_cc_set_link_backend   (c, &minimal_link);
    zero_cc_set_memory_backend (c, &minimal_memory);
    zero_cc_set_abi_backend    (c, &minimal_abi);
    zero_cc_set_atomic_backend (c, &minimal_atomic);
    zero_cc_set_generic_backend(c, &minimal_generic);
    zero_cc_set_stdlib_backend (c, &minimal_stdlib);
    zero_cc_set_diag_backend   (c, &minimal_diag);
    /* TLS backend remains the default static alias */
    zero_cc_set_tls_backend(c, NULL);
}
