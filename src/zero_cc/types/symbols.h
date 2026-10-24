#ifndef ZERO_CC_TYPES_SYMBOLS_H
#define ZERO_CC_TYPES_SYMBOLS_H

#include "zero_cc/isa/isa_backend.h"

#include <stddef.h>
#include <stdint.h>
#include "zero_cc/core/compiler_state.h"
#include "zero_cc/core/buffers.h"
#include "zero_cc/types/token.h"
#include "zero_cc/types/type_system.h"
#include "zero_cc/semantic/expr.h"
#include "zero_cc/semantic/diagnostics.h"
#include "zero_cc/ir/regalloc.h"
#include "zero_cc/ir/lowering_state.h"
#include "zero_cc/ir/label_patcher.h"
#include "zero_cc/ir/ir_arena.h"
#include "zero_cc/opt/opt_pass.h"
#include "zero_cc/opt/cfg.h"

/* Compiler & Emitter Context */
typedef struct ZeroCCStrChunk ZeroCCStrChunk;

/* Static TLS variable descriptor (declaration order). The TLS backend's
 * finalize() lays these out into the .tdata/.tbss block. */
typedef struct {
    char name[128];
    size_t size;
    size_t align;
    bool has_init;
    int64_t init;
} ZeroCCTLSVar;

typedef struct ZeroCCCompiler {
    ZeroCCToken *tokens;
    size_t token_count;
    size_t token_pos;
    int last_expr_is_float;

    ZeroCCSym *locals;
    size_t local_count;
    size_t local_capacity;
    int32_t *local_hash_buckets;
    size_t   local_hash_bucket_count;
    int32_t *local_hash_next;
    /* Block scope (C11 6.2.1): when lowering a NODE_BLOCK, local lookup is
     * capped at this many local symbols (visible indices 0..bound-1).
     * Symbols at index >= bound were declared in a LATER sibling block and
     * must not shadow references in the current block. 0 = unbounded.
     * Set from NODE_BLOCK->aux.step (parse-time local_count + 1). */
    uint32_t local_lookup_bound;
    int32_t current_stack_frame;
    size_t static_local_seq;              /* Sprint 37: unique mangle counter for static locals */

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
    /* Static TLS block (_Thread_local, x86-64).  AOT: .tdata (initialized)
     * followed by .tbss (uninitialized); finalized by the TLS backend after
     * all top-level declarations.  JIT: the block lives in the data section
     * and TLS variables are ordinary globals (jit_mode = 1). */
    ZeroCCBuffer tls_data_section;        /* .tdata contents (AOT) */
    size_t tls_bss_size;                  /* .tbss size (AOT) */
    size_t tls_block_align;               /* .tdata/.tbss section alignment */
    // Thread‑local (static alias) storage – per compiler instance
    struct {
        char name[128];
        int64_t value;
        ZeroCCType type;
    } thread_locals[64];
    size_t thread_local_count;
    /* TLS variable descriptors (declaration order) used by finalize to lay
     * out the .tdata/.tbss block and patch each global's data_offset. */
    ZeroCCTLSVar tls_vars[64];
    size_t tls_var_count;
    /* Backend pointers */
    const ZeroCCTLSBackend *tls_backend;  /* TLS variable handling */
    int jit_mode;                         /* 1 when compiling for --run JIT */
    int had_error;
    /* Legacy display buffer. Sized to hold a formatted message plus the
     * "(near '<token>')" suffix that diag_report_error_at_token appends
     * (see full_buf in diagnostics.c, 320B). */
    char error_msg[512];
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
    bool dump_cfg; /* Dump CFG after lowering (-dump-cfg) */
    bool emit_asm; /* Emit assembly (-S flag) */
    FILE *asm_out; /* Assembly output file (NULL = stdout) */

    /* CFG layer cache (Sprint 35 Phase 2): lazily built view over the IR,
     * invalidated whenever cfg_version is bumped (ir_reset, optimizer
     * passes, callee-save append). */
    ZeroCCCFG cfg_cache;
    uint64_t cfg_version;

    /* Optimization level (from -O flag) */
    OptLevel opt_level;

    /* Frame pointer policy (from -fomit-frame-pointer / -fno-omit-frame-pointer):
     * -1 = default (omit for fixed-frame functions at -O1 and above),
     *  0 = always keep the frame pointer,
     *  1 = always omit when the frame is static. */
    int omit_frame_pointer;

    /* IR arena - dynamic instruction buffer */
    IRArena ir_arena;              /* IR instruction arena */
    IRCallArgsArena ir_call_args;  /* Call argument tracking arena */

    /* Translation-unit level AST arena for inlinable functions */
    void *tu_ast_arena;

    /* Lowering state - Task 61/62: Encapsulated per-compiler-instance state */
    LoweringState lowering;

    /* Label patcher state - Task 62: Unified label resolution */
    LabelPatcherState label_patcher;

    /* Stable string pool (chunked): interned copies of identifiers, labels,
     * member names and string literals referenced by AST nodes. Chunks are
     * never realloc'd, so interned pointers stay valid across token-buffer
     * growth (which moves the ZeroCCToken.str_val buffers they copied from). */
    struct ZeroCCStrChunk *str_chunks;
} ZeroCCCompiler;

/* Forward declaration for self-referential struct */
typedef struct ZeroCCStrChunk ZeroCCStrChunk;

/* Chunked stable string pool. AST nodes store pointers into these chunks
 * (zero-copy into the token buffer is unsafe: c->tokens is realloc'd when
 * it outgrows its capacity, dangling every previously captured str_val). */
struct ZeroCCStrChunk {
    char *data;
    size_t len;
    size_t cap;
    struct ZeroCCStrChunk *next;
};

/* String pool functions (implemented in string_pool.c) */
extern const char *zero_cc_intern_string(ZeroCCCompiler *c, const char *s);
extern void zero_cc_free_string_pool(ZeroCCCompiler *c);

/* Full teardown of a compiler instance (implemented in
 * core/compiler_free.c): frees tokens, locals/hash tables, struct member
 * arrays, IR arenas, the TU AST arena, the string pool, regalloc, the CFG
 * cache, and the code sections, then the struct itself. Call after you no
 * longer need the compiled code; `asm_out` is caller-owned and not closed.
 * (The compile API has no other destroy path — without this, every
 * multi-compile consumer leaks one IR arena per compile.) */
extern void zero_cc_compiler_free(ZeroCCCompiler *c);

int64_t decl_align_spec; /* alignment specifier from _Alignas */


#endif /* ZERO_CC_TYPES_SYMBOLS_H */
