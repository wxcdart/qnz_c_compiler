#ifndef ZERO_CC_TYPES_SYMBOLS_H
#define ZERO_CC_TYPES_SYMBOLS_H

#include "../isa/isa_backend.h"

#include <stddef.h>
#include <stdint.h>
#include "zero_cc/core/compiler_state.h"
#include "zero_cc/core/buffers.h"
#include "zero_cc/types/token.h"
#include "zero_cc/types/type_system.h"
#include "zero_cc/semantic/expr.h"
#include "zero_cc/semantic/diagnostics.h"
#include "../ir/regalloc.h"
#include "../ir/lowering_state.h"
#include "../ir/label_patcher.h"
#include "../ir/ir_arena.h"

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

    ZeroCCEnumDef enums[ZERO_CC_MAX_ENUM_DEFS];
    size_t enum_count;

    ZeroCCEnumConst enum_constants[ZERO_CC_MAX_ENUMS];
    size_t enum_const_count;

    ZeroCCFunction functions[ZERO_CC_MAX_FUNCTIONS];
    size_t function_count;

    ZeroCCReloc relocs[ZERO_CC_MAX_RELOCS];
    size_t reloc_count;

    ZeroCCStringReloc str_relocs[ZERO_CC_MAX_STR_RELOCS];
    size_t str_reloc_count;

    ZeroCCDataReloc data_relocs[ZERO_CC_MAX_DATA_RELOCS];
    size_t data_reloc_count;

    /* Register allocator state */
    RegAllocator regalloc;                /* Linear-scan register allocator */
    int next_temp_id;                     /* Next temporary variable ID */
    uint16_t ir_next_label;               /* Next IR label index */
    SemContext *sem_ctx;                  /* Semantic context for type info */

    /* Label resolution for jump patching */
    struct {
        const char *name;
        size_t offset;
        size_t patches[16];
        int patch_count;
    } labels[32];
    int label_count;

    /* ISA backend */
    ZeroCCISA isa;                        /* Target ISA */
    const ZeroCCISABackend *isa_backend;  /* ISA-specific codegen backend */

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
    const ZeroCCTLSBackend *tls_backend;  /* TLS variable handling */
    int had_error;
    char error_msg[256];
    int expr_depth;                       /* Expression parser recursion depth counter */

    /* Rich diagnostics system */
    DiagBuf diag_buf;
    Arena *diag_arena;

    /* Pragma pack alignment (0 = default) */
    int default_pack;
    /* #pragma pack events recorded by the tokenizer: from token index
     * pack_ev_tok[i] onward the pack value is pack_ev_val[i]. */
    size_t pack_ev_tok[256];
    int pack_ev_val[256];
    size_t pack_ev_n;
    
    /* GCC diagnostic pragma state (from preprocessor) */
    struct {
        bool warning_as_error;
        char *disabled_warnings[64];
        int disabled_warning_count;
    } diag_pragma_state;
    
    /* Debug flags */
    bool dump_ir;  /* Dump IR after lowering */
    bool emit_asm; /* Emit assembly (-S flag) */
    FILE *asm_out; /* Assembly output file (NULL = stdout) */

    /* IR arena - dynamic instruction buffer */
    IRArena ir_arena;              /* IR instruction arena */
    IRCallArgsArena ir_call_args;  /* Call argument tracking arena */

    /* Lowering state - Task 61/62: Encapsulated per-compiler-instance state */
    LoweringState lowering;
    
    /* Label patcher state - Task 62: Unified label resolution */
    LabelPatcherState label_patcher;
} ZeroCCCompiler;

int64_t decl_align_spec; /* alignment specifier from _Alignas */


#endif /* ZERO_CC_TYPES_SYMBOLS_H */
