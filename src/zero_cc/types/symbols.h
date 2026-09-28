#ifndef ZERO_CC_TYPES_SYMBOLS_H
#define ZERO_CC_TYPES_SYMBOLS_H

/* Compiler & Emitter Context */
typedef struct ZeroCCCompiler {
    ZeroCCToken *tokens;
    size_t token_count;
    size_t token_pos;
    int last_expr_is_float;

    ZeroCCSym locals[ZERO_CC_MAX_LOCALS];
    size_t local_count;
    int32_t current_stack_frame;

    ZeroCCTypedef typedefs[ZERO_CC_MAX_TYPEDEFS];
    size_t typedef_count;

    ZeroCCStructDef structs[ZERO_CC_MAX_STRUCTS];
    size_t struct_count;

    ZeroCCEnumConst enum_constants[ZERO_CC_MAX_ENUMS];
    size_t enum_const_count;

    ZeroCCFunction functions[ZERO_CC_MAX_FUNCTIONS];
    size_t function_count;

    ZeroCCReloc relocs[ZERO_CC_MAX_RELOCS];
    size_t reloc_count;

    ZeroCCStringReloc str_relocs[ZERO_CC_MAX_STR_RELOCS];
    size_t str_reloc_count;

    ZeroCCBuffer text_section;            /* Machine bytecode (.text) */
    ZeroCCBuffer rodata_section;          /* Read-only string constants (.rodata) */
    ZeroCCBuffer data_section;            /* Initialized globals (.data) */
    // Thread‑local (static alias) storage – per compiler instance
    struct {
        char name[128];
        int64_t value;
        ZeroCCType type;
    } thread_locals[64];
    size_t thread_local_count;
    /* Backend pointers */
    const ZeroCCTLSBackend *tls_backend;
    const ZeroCCEmitBackend   *emit_backend;
    const ZeroCCLinkBackend   *link_backend;
    const ZeroCCMemoryBackend *memory_backend;
    const ZeroCCABICallBackend *abi_backend;
    const ZeroCCAtomicBackend *atomic_backend;
    const ZeroCCGenericBackend *generic_backend;
    const ZeroCCStdlibBackend *stdlib_backend;
    const ZeroCCDiagBackend   *diag_backend;
    int had_error;
    char error_msg[256];
} ZeroCCCompiler;

int64_t decl_align_spec; /* alignment specifier from _Alignas */


#endif /* ZERO_CC_TYPES_SYMBOLS_H */
