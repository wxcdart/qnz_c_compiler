#ifndef ZERO_CC_TYPES_SYMBOLS_H
#define ZERO_CC_TYPES_SYMBOLS_H

#include "zero_cc/isa/isa_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
typedef struct ZeroCCGlobalVar ZeroCCGlobalVar;

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
    /* GNU __int128 (Sprint 109): 1 if the most recent lower_expr produced a
     * 128-bit (lo,hi) GPR128 value (a 128-bit local/global load, a 128-bit
     * arithmetic binop/unop, or a 128-bit cast); 0 if it produced a plain
     * 64-bit temp (a constant, a narrow value, a boolean, ...). Consumers
     * (assignment, binop) use it to decide whether to widen to GPR128. */
    int last_expr_is_128;
    /* Pointer stars consumed by the most recent zero_cc_parse_type_name
     * call (declarator stars immediately following the base specifier,
     * e.g. the `*` in `char *a, *b;`).  Multi-declarator parsing uses this
     * to recover the pre-star base type. */
    int last_type_ptr_depth;

    /* GNU __attribute__((section("name"))): set by the file-scope loop when a
     * `__zc_section__("name")` marker is found on the current declaration.  Consumed
     * by add_global_var (objects) and by the function registration (Sprint 108,
     * functions) to place the entity in the named ELF section (AOT -c). */
    char pending_section[64];

    ZeroCCSym *locals;
    size_t local_count;
    size_t local_capacity;
    int32_t *local_hash_buckets;
    size_t   local_hash_bucket_count;
    int32_t *local_hash_next;
    /* Block scope (C11 6.2.1): when lowering a NODE_BLOCK, local lookup is
     * capped at this many local symbols (visible indices 0..bound-1).
     * Symbols at index >= bound were declared in a LATER sibling block and
     * must not shadow references in the current block. -1 = unbounded.
     * Set from NODE_BLOCK->aux.step (parse-time local_count + 1). */
    int32_t local_lookup_bound;
    int32_t current_stack_frame;
    size_t static_local_seq;              /* Sprint 37: unique mangle counter for static locals */
    size_t anon_struct_seq;               /* unique name counter for anonymous struct/union definitions */
    ZeroCCType (*parse_struct_body)(struct ZeroCCCompiler *c, bool is_union);
    ZeroCCTypedef typedefs[ZERO_CC_MAX_TYPEDEFS];
    size_t typedef_count;

    ZeroCCStructDef structs[ZERO_CC_MAX_STRUCTS];
    size_t struct_count;

    ZeroCCEnumDef enums[ZERO_CC_MAX_ENUM_DEFS];
    size_t enum_count;

    /* Enum constants: open-addressing hash table (string -> value).
     * Power-of-two capacity; empty slots have name[0] == '\0'.
     * Grows on load factor > 0.7.  O(1) registration and lookup. */
    ZeroCCEnumConst *enum_constants;     /* dynamically allocated table */
    size_t enum_const_capacity;          /* power of two */
    size_t enum_const_count;             /* active entries */

    ZeroCCFunction functions[ZERO_CC_MAX_FUNCTIONS];
    size_t function_count;
    Arena *fn_arena;                      /* Arena for function parameter slices */

    /* Function-name hash backing zero_cc_find_function (Sprint 67):
     * covers entries [0, fn_hash_upto), incrementally updated - the table
     * is append-only.  Without it, a TU with 100k extern declarations
     * (c-torture compile/limits-externdecl) pays O(n^2) strcmps. */
    int32_t *fn_hash;                     /* open addressing; -1 = empty */
    int32_t *fn_hash_next;                /* same-name chain per entry; -1 end */
    size_t fn_hash_cap;                   /* power of two */
    size_t fn_hash_upto;                  /* entries covered by the hash */

    size_t nested_fn_base;

    /* Explicit identity for the next zero_cc_lower_ast call: the index in
     * c->functions of the function whose body is about to be lowered.
     * When unset, the heuristic (most recently registered non-prototype)
     * is used. Needed because nested functions registered while PARSING
     * the enclosing body sit on top of the enclosing function in
     * c->functions, so the heuristic would capture the nested one (losing
     * main's implicit-`return 0` init and mis-attributing diagnostics). */
    bool has_pending_lowering_fn;
    size_t pending_lowering_fn_idx;

    /* Set while re-parsing a nested function's body: the by-reference
     * capture flags of the ENCLOSING function. A nested function capturing
     * another nested function's by-ref capture POINTER PARAM (same name as
     * the mirror local that shadows it) must rename that param to a hidden
     * name, or the duplicate name resolves to the pointer and `x += 4`
     * lowers as pointer arithmetic (c-torture execute/20061220-1.c). */
    const bool *enc_byref_captures;
    size_t enc_capture_count;

    /* Phase 2: nested function trampoline tracking */
    size_t nested_trampoline_count;        /* Number of trampolines allocated */

    ZeroCCReloc relocs[ZERO_CC_MAX_RELOCS];
    size_t reloc_count;

    /* Latch: the text reloc table overflowed and the error was reported once.
     * Emitters keep their silent-drop guard (compilation continues so the
     * user sees one clear error, not a cascade), but must call
     * zero_cc_reloc_overflow_note() on the drop path. */
    int reloc_overflow_reported;

    ZeroCCStringReloc str_relocs[ZERO_CC_MAX_STR_RELOCS];
    size_t str_reloc_count;

    ZeroCCDataReloc data_relocs[ZERO_CC_MAX_DATA_RELOCS];
    size_t data_reloc_count;

    /* Global variable table & name hash (Sprint 77: migrated from file-scope globals) */
    ZeroCCGlobalVar *global_vars;
    size_t global_var_count;
    size_t global_var_capacity;
    size_t g_name_hash_cap;
    size_t *g_name_hash;
    uint32_t *g_name_hash_sig;

    /* Register allocator state */
    RegAllocator regalloc;                /* Linear-scan register allocator */
    int next_temp_id;                     /* Next temporary variable ID */
    uint16_t ir_next_label;               /* Next IR label index */
    /* Sprint 118 (type-system hardening #1): per-temp value-type tag table,
     * indexed by temp id (0..65534); lazily allocated, reset in ir_reset.
     * Stamped at lowering from the AST node's semantic TypeInfo (the
     * frontend's verdict) and cross-checked against the backend width marks
     * by tools/type_tag_verifier.py + the per-expression flag check in
     * lower_expr. Slice 1: emitted, NOT consumed by codegen. */
    uint8_t *temp_type_tags;
    uint32_t temp_type_tags_hi;           /* High-water mark of tagged ids */
    size_t type_tag_mismatches;           /* flag/tag disagreement count (Sprint 118) */
    SemContext *sem_ctx;                  /* Semantic context for type info */

    /* Label resolution for jump patching */
    struct {
        const char *name;
        size_t offset;
        size_t patches[16];
        int patch_count;
    } labels[32];
    int label_count;

    /* User label text offsets for JIT data-reloc resolution (static &&label tables) */
    struct {
        size_t func_idx;
        uint16_t ir_label;
        size_t text_offset;
    } user_label_offsets[256];
    size_t user_label_offset_count;

    /* Sprint 99: nonlocal goto absolute-jump relocations (AOT .o only):
     * an adrp/auipc at (text_patch_offset - 4) and an add/jalr at
     * text_patch_offset targeting a local label symbol at
     * text_target_offset (the ancestor's label position in .text). */
    struct {
        size_t text_patch_offset;    /* the add / jalr instruction */
        size_t text_target_offset;   /* label position in .text */
    } nl_goto_relocs[64];
    size_t nl_goto_reloc_count;

    /* ISA backend */
    ZeroCCISA isa;                        /* Target ISA */
    const ZeroCCISABackend *isa_backend;  /* ISA-specific codegen backend */

    ZeroCCBuffer text_section;            /* Machine bytecode (.text) */
    ZeroCCBuffer rodata_section;          /* Read-only string constants (.rodata) */
    ZeroCCBuffer data_section;            /* Initialized globals (.data) */
    size_t bss_size;                      /* .bss size */
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

    /* -fno-builtin control (c-torture execute/builtins: the abs/labs
     * builtin lowering must fall back to a real call when the flag is set
     * and the user declares the function). */
    bool no_builtin_all;          /* -fno-builtin */
    int no_builtin_count;         /* -fno-builtin-<name> entries */
    char no_builtin_names[16][32];
    int sanitize_overflow;        /* -fsanitize=undefined: emit OP_OVF_TRAP after signed add/sub/mul */

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

/* Report (at most once per compilation) that the TU's text-relocation
 * table is full and a further reloc was dropped. A dropped reloc leaves an
 * unpatched immediate in the code image (silent miscompile / segfault), so
 * the drop must surface as a clean compile error instead of vanishing.
 * Callers keep their silent-drop guard so the rest of the TU still emits
 * and the user gets exactly one clear diagnostic. */
static inline void zero_cc_reloc_overflow_note(ZeroCCCompiler *c) {
    if (c->reloc_overflow_reported) return;
    c->reloc_overflow_reported = 1;
    c->had_error = 1;
    snprintf(c->error_msg, sizeof(c->error_msg),
             "too many relocations in this translation unit (limit %d); "
             "split the file into smaller units", (int)ZERO_CC_MAX_RELOCS);
}

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
/* Length-aware intern for strings that may contain embedded NUL bytes
 * (string literals); copies exactly len content bytes + terminating NUL. */
extern const char *zero_cc_intern_string_len(ZeroCCCompiler *c, const char *s, size_t len);
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
