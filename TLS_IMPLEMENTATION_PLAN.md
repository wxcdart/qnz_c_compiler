# Cross-Platform JIT-Managed TLS Implementation Plan

## Overview

Implement full C99 `_Thread_local` support using **system TLS** where available:

| Platform | C Library | TLS Access |
|----------|-----------|------------|
| **Linux x86-64** | musl/glibc | `fs` segment |
| **Linux aarch64/riscv64** | musl/glibc | `x28`/`tp` register |
| **Windows x86-64** | UCRT | `gs` segment |
| **macOS x86-64/aarch64** | libc | `gs` segment / `x28` |
| **Baremetal aarch64/riscv64** | None | Custom TLSBlock |

## Architecture (System TLS)

**Use system TLS where available**, custom TLSBlock for baremetal:

### With C Library (Linux/Windows/macOS)

```c
// Linux (musl): fs segment
mov rax, fs:[offset]

// Windows/macOS: gs segment  
mov rax, gs:[offset]

// aarch64: x28 register
ldr x0, [x28, offset]

// riscv64: tp register
lw a0, [tp, offset]
```

### Baremetal (No C Library)

```c
// Custom TLSBlock per thread
typedef struct {
    void* tls_storage[64];
    int tls_count;
} TLSBlock;

// Use a reserved register for TLS base (e.g., x27/r14)
mov r15, [tls_base_ptr]    // Load TLS block address
mov rax, [r15 + offset]    // Load variable
```

## Phase 1: Backend TLS Access Functions

### Files to Create/Modify

1. **`src/zero_cc/isa/x86_64.c`** - Add TLS load/store/address functions
2. **`src/zero_cc/isa/aarch64.c`** - Add TLS load/store/address functions  
3. **`src/zero_cc/isa/riscv.c`** - Add TLS load/store/address functions

No custom TLS infrastructure needed - use system TLS directly.

## Phase 2: Backend TLS Access Functions

### x86-64 (Linux)

**Use `fs` segment** (musl/glibc TLS segment):

```c
// Load TLS variable
mov rax, fs:[offset]        // Direct access via fs segment

// Store TLS variable
mov fs:[offset], rax        // Direct access via fs segment

// Address of TLS variable (requires base register)
mov rax, fs:[offset]        // Get value
// For address, need to compute: &tls_var = fs_base + offset
```

### x86-64 (Windows)

**Use `gs` segment** (UCRT TLS segment):

```c
// Same pattern, different segment
mov rax, gs:[offset]
mov gs:[offset], rax
```

### x86-64 (macOS)

**Use `gs` segment** (libc TLS segment):

```c
// Same as Windows
mov rax, gs:[offset]
mov gs:[offset], rax
```

### aarch64 (Linux/musl)

**Use `x28`** (TLS register per ABI):

```c
// Load TLS variable
ldr x0, [x28, offset]       // x28 is TLS base

// Store TLS variable
str x0, [x28, offset]

// Address of TLS variable
add x0, x28, offset
```

### aarch64 (Baremetal)

**Use custom TLSBlock** with reserved register (e.g., `x27`):

```c
// Load TLS block pointer
ldr x27, [tls_base_ptr]     // x27 = TLS base
// Load TLS variable
ldr x0, [x27, offset]

// Store TLS variable
str x0, [x27, offset]

// Address of TLS variable
add x0, x27, offset
```

### riscv64 (Linux/musl)

**Use `tp` (x8)** (TLS register):

```c
// Load TLS variable
lw a0, [tp, offset]         // tp is TLS base

// Store TLS variable
sw a0, [tp, offset]

// Address of TLS variable
add a0, tp, offset
```

### riscv64 (Baremetal)

**Use custom TLSBlock** with reserved register (e.g., `s10/x26`):

```c
// Load TLS block pointer
lw s10, [tls_base_ptr]      // s10 = TLS base
// Load TLS variable
lw a0, [s10, offset]

// Store TLS variable
sw a0, [s10, offset]

// Address of TLS variable
add a0, s10, offset
```

## Phase 3: Backend Integration

### Modify `src/zero_cc/tls_backend.c`

Replace JIT-managed with real JIT-managed TLS:

```c
static void* jit_tls_declare(ZeroCCCompiler* c, const char* name,
                             const ZeroCCType* type, int64_t init) {
    // Allocate slot in current TLS block
    int idx = c->thread_local_count++;
    c->thread_locals[idx].name = strdup(name);
    c->thread_locals[idx].init_value = init;
    c->thread_locals[idx].offset = idx * sizeof(void*);
    return &c->thread_locals[idx];
}

static void jit_tls_emit_load(ZeroCCCompiler* c, const void* handle) {
    struct tls_slot* slot = (struct tls_slot*)handle;
    // Backend-specific: emit code to load from [TLS_BASE + offset]
    BE(c)->emit_load_tls(c, slot->offset);
}

static void jit_tls_emit_store(ZeroCCCompiler* c, const void* handle) {
    struct tls_slot* slot = (struct tls_slot*)handle;
    // Backend-specific: emit code to store to [TLS_BASE + offset]
    BE(c)->emit_store_tls(c, slot->offset);
}

static void jit_tls_emit_address(ZeroCCCompiler* c, const void* handle) {
    struct tls_slot* slot = (struct tls_slot*)handle;
    // Backend-specific: emit code to load address [TLS_BASE + offset]
    BE(c)->emit_address_tls(c, slot->offset);
}
```

### Modify Each Backend (`isa/x86_64.c`, `isa/aarch64.c`, `isa/riscv.c`)

Add TLS emission functions:

```c
// x86_64.c
static void x86_64_emit_load_tls(ZeroCCCompiler* c, size_t offset) {
    // mov r15, [tls_block_ptr]
    zero_cc_emit_u8(&c->text_section, 0x4C);  // REX.W + REX.RB
    zero_cc_emit_u8(&c->text_section, 0x8B);  // mov r15, [rip + disp32]
    // ... emit tls_block_ptr address
    // mov rax, [r15 + offset]
    // ...
}

static void x86_64_emit_store_tls(ZeroCCCompiler* c, size_t offset) {
    // mov r15, [tls_block_ptr]
    // mov [r15 + offset], rax
    // ...
}

static void x86_64_emit_address_tls(ZeroCCCompiler* c, size_t offset) {
    // mov r15, [tls_block_ptr]
    // lea rax, [r15 + offset]
    // ...
}
```

## Phase 4: Runtime Support

### `src/zero_cc/tls_runtime.c`

```c

#include <stdlib.h>

__thread TLSBlock* zero_cc_tls_block = NULL;

void zero_cc_tls_init(void) {
    zero_cc_tls_block = calloc(1, sizeof(TLSBlock));
}

void zero_cc_tls_destroy(void) {
    free(zero_cc_tls_block);
    zero_cc_tls_block = NULL;
}

// Thread initialization

 {
    // No pthread needed - user calls zero_cc_tls_init() manually;
}

// Or use __thread initializer
__thread TLSBlock* zero_cc_tls_block = NULL;
```

## Phase 5: Test Coverage

### New Tests

1. **`test/tls_read.c`** - Read-only TLS
2. **`test/tls_write.c`** - Write to TLS
3. **`test/tls_address.c`** - Address-of TLS variable
4. **`test/tls_multi_thread.c`** - Multiple threads (if pthread support)
5. **`test/tls_jit.c`** - Existing JIT test (update to verify)

### Test Cases

```c
// test/tls_read.c
_Thread_local int x = 5;
int foo() { return x; }  // Should return 5

// test/tls_write.c
_Thread_local int x = 5;
int foo() { x = 10; return x; }  // Should return 10

// test/tls_address.c
_Thread_local int x = 5;
int foo() { int* p = &x; return *p; }  // Should return 5

// test/tls_multi_thread.c
_Thread_local int counter = 0;
void* thread_func(void* arg) {
    counter++;
    return (void*)(intptr_t)counter;
}
// Each thread should see counter = 1
```

## Implementation Order

1. ✅ **Done**: Remove unused backend scaffolding
2. **Phase 1**: Add TLS emission to x86_64 backend
3. **Phase 2**: Add TLS emission to aarch64 backend
4. **Phase 3**: Add TLS emission to riscv backend
5. **Phase 4**: Update `tls_backend.c` to use system TLS
6. **Phase 5**: Add tests and verify
7. **Phase 6**: Apply same pattern to atomic operations
8. **Phase 7**: Apply same pattern to pragmas

---

## Atomic Operations (Same Pattern)

### Platform-Specific Atomic Support

| Platform | C Library | Atomic Implementation |
|----------|-----------|----------------------|
| **Linux/Windows/macOS** | musl/UCRT/libc | libatomic or built-in |
| **Baremetal** | None | Custom (LOCK/LDXR/AMOSWAP) |

### Backend Structure

```c
// src/zero_cc/zero_cc_backends.h - Keep atomic interface
typedef struct ZeroCCAtomicBackend {
    void (*emit_load)(struct ZeroCCCompiler* c, const struct ZeroCCType* type, 
                      uint8_t src_reg, uint8_t* dst_reg);
    void (*emit_store)(struct ZeroCCCompiler* c, const struct ZeroCCType* type,
                       uint8_t src_reg, uint8_t dst_reg);
    void (*emit_rmw)(struct ZeroCCCompiler* c, const struct ZeroCCType* type,
                     uint8_t addr_reg, uint8_t val_reg, int op, uint8_t* out_reg);
} ZeroCCAtomicBackend;

// Each backend implements platform-specific atomics
// x86_64.c: LOCK prefix instructions
// aarch64.c: LDXR/STXR instructions
// riscv.c: AMO instructions or fallback
```

### Implementation Examples

```c
// x86_64.c - Atomic add with LOCK prefix
static void x86_64_emit_atomic_add(ZeroCCCompiler* c, uint8_t addr_reg, uint8_t val_reg) {
    zero_cc_emit_u8(&c->text_section, 0xF0);     // LOCK prefix
    zero_cc_emit_u8(&c->text_section, 0x48);     // REX.W
    zero_cc_emit_u8(&c->text_section, 0x01);     // add [raddr], rsrc
    // ...
}

// aarch64.c - Atomic add with LDXR/STXR loop
static void aarch64_emit_atomic_add(ZeroCCCompiler* c, uint8_t addr_reg, uint8_t val_reg) {
    // LDXR xtmp, [addr]
    // ADD xtmp, xtmp, val
    // STXR wtmp, xtmp, [addr]
    // CBZ wtmp, done
    // B retry
    // ...
}

// riscv.c - Atomic add with AMOSWAP/AMOADD
static void riscv_emit_atomic_add(ZeroCCCompiler* c, uint8_t addr_reg, uint8_t val_reg) {
    // AMOADD.w a0, val, (addr)
    // ...
}
```

---

## Pragmas (Same Pattern)

### Platform-Specific Pragmas

| Pragmas | Linux/Windows/macOS | Baremetal |
|---------|---------------------|-----------|
| `#pragma pack` | Supported | Supported |
| `#pragma once` | Supported | Supported |
| `#pragma comment(lib)` | Windows only | N/A |
| `#pragma clang` | Compiler-specific | N/A |
| `#pragma GCC` | Compiler-specific | N/A |
| `#pragma simd` | SIMD support | Custom implementation |
| `#pragma omp` | OpenMP runtime | Custom or stub |

### Backend Structure

```c
// src/zero_cc/zero_cc_backends.h - Keep pragma interface  
typedef struct ZeroCCPragmaBackend {
    void (*handle_pragma)(struct ZeroCCCompiler* c, const char* name, const char* args);
    bool (*is_supported)(const char* name);
} ZeroCCPragmaBackend;

// Each backend implements platform-specific pragmas
// x86_64.c: x86-specific pragmas
// aarch64.c: ARM-specific pragmas
// common.c: Cross-platform pragmas (pack, once)
```

### Implementation Examples

```c
// common.c - Cross-platform pragmas
static void handle_pragma_pack(ZeroCCCompiler* c, const char* args) {
    // Parse #pragma pack(n) and update alignment
    int align = atoi(args);
    c->current_pack_align = align;
}

// x86_64.c - x86-specific pragmas  
static void handle_pragma_comment(ZeroCCCompiler* c, const char* args) {
    // #pragma comment(lib, "foo.lib") - Windows only
    if (strstr(args, "lib")) {
        // Record library dependency
    }
}
```

---

## Unified Backend Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    ZeroCCCompiler                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │ isa_backend │  │ tls_backend │  │ atomic_backend │  │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │
│  │pragma_backend│ │ abi_backend │  │ ...          │      │
│  └─────────────┘  └─────────────┘  └─────────────┘      │
└─────────────────────────────────────────────────────────┘
         │                │                │
         ▼                ▼                ▼
    x86_64.c         tls_backend.c    x86_64.c
    aarch64.c        (system TLS)     aarch64.c
    riscv.c                           riscv.c
```

**Pattern**: Each feature gets a backend interface + per-ISA implementation.
**Benefit**: Clean separation, easy to add platform-specific behavior.
**Trade-off**: More files, but better maintainability.

## File Changes Summary

| File | Action | Lines |
|------|--------|-------|
| `src/zero_cc/tls_backend.c` | Modify | ~50 |
| `src/zero_cc/isa/x86_64.c` | Modify | +30 |
| `src/zero_cc/isa/aarch64.c` | Modify | +30 |
| `src/zero_cc/isa/riscv.c` | Modify | +30 |
| `test/tls_read.c` | New | ~15 |
| `test/tls_write.c` | New | ~15 |
| `test/tls_address.c` | New | ~15 |

**Total**: ~150 lines added, ~50 lines modified

**No new files needed** - musl handles TLS runtime!

## C99 Conformance Checklist

- [x] `_Thread_local` storage class specifier
- [x] Thread-local initialization
- [x] Thread-local read access
- [x] Thread-local write access
- [x] Address-of operator on TLS variables
- [ ] Thread-safe initialization (C11 `call_once`)
- [ ] `_Atomic _Thread_local` (C11 atomics)

## Notes

- **Platform-specific TLS** - different C library per platform
- **Linux** - musl/glibc handles TLS, use `fs` segment or `x28`/`tp`
- **Windows** - UCRT handles TLS, use `gs` segment
- **macOS** - libc handles TLS, use `gs` segment or `x28`
- **Baremetal** - No C library, need custom TLSBlock implementation
- **Performance** - direct segment access, no extra overhead

---

## Other C99/C11/C2X Features to Consider

### High Priority (Backend-Dependent)

| Feature | Standard | Backend Needed | Platform-Specific? |
|---------|----------|----------------|-------------------|
| **`_Alignas` / `alignof`** | C11 | `abi_backend` | Yes (ABI alignment rules) |
| **`_Atomic`** | C11 | `atomic_backend` | Yes (LOCK/LDXR/AMO) |
| **`_Thread_local`** | C11 | `tls_backend` | Yes (fs/gs/x28/tp) |
| **`inline` semantics** | C99/C11 | `isa_backend` | Yes (linkage rules) |
| **`restrict`** | C99 | `abi_backend` | No (optimization hint) |
| **Complex numbers** | C99 | `isa_backend` | Yes (float ops) |
| **`_Static_assert`** | C11 | Parser + `isa_backend` | No |
| **`_Generic`** | C11 | Preprocessor | No |

### Medium Priority

| Feature | Standard | Backend Needed | Platform-Specific? |
|---------|----------|----------------|-------------------|
| **`_Noreturn`** | C11 | `isa_backend` | No |
| **`_Alignof`** | C11 | `abi_backend` | Yes |
| **Flexible array members** | C99 | `isa_backend` | No |
| **Designated initializers** | C99 | Parser | No |
| **Static inline** | C11 | Linker | Yes |
| **`long long`** | C99 | `isa_backend` | Yes (64-bit ops) |
| **Fixed-width types** | C99 | Type system | No |

### Low Priority (Preprocessor/Parser)

| Feature | Standard | Implementation |
|---------|----------|----------------|
| **Variadic macros** | C99 | Preprocessor |
| **`__func__`** | C99 | Parser |
| **Predefined macros** | C99/C11 | Preprocessor |

---

## Recommended Backend Architecture

```c
// Complete backend set for C11 conformance
typedef struct {
    // ISA-specific code generation
    ZeroCCEmitBackend   *isa_backend;     // x86_64, aarch64, riscv
    
    // Memory and ABI
    ZeroCCABICallBackend *abi_backend;    // Calling convention, alignment
    
    // Thread support
    ZeroCCTLSBackend    *tls_backend;     // _Thread_local
    
    // Atomic operations
    ZeroCCAtomicBackend *atomic_backend;  // _Atomic
    
    // Pragmas and attributes
    ZeroCCPragmaBackend *pragma_backend;  // #pragma pack, etc.
    
    // Memory management
    ZeroCCMemoryBackend *memory_backend;  // malloc/free hooks
    
    // Diagnostics
    ZeroCCDiagBackend   *diag_backend;    // Error/warning reporting
} ZeroCCCompiler;
```

## Priority Order

1. **TLS** (`_Thread_local`) - C11, already planned ✅
2. **Atomic** (`_Atomic`) - C11, similar to TLS
3. **ABI** (`_Alignas`, `alignof`) - C11, needed for correct layout
4. **Inline** (`inline`, `static inline`) - C99/C11, linkage semantics
5. **Complex** (`_Complex`) - C99, optional but useful
6. **Pragmas** (`#pragma pack`) - C99, widely used

## Implementation Pattern

All features follow the same pattern:

```c
// 1. Define backend interface in zero_cc_backends.h
typedef struct FeatureBackend {
    void (*feature_op)(ZeroCCCompiler* c, ...);
} FeatureBackend;

// 2. Add to compiler struct
struct ZeroCCCompiler {
    FeatureBackend *feature_backend;
};

// 3. Implement per-ISA
// isa/x86_64.c: feature_x86_64_op(...)
// isa/aarch64.c: feature_aarch64_op(...)
// isa/riscv.c: feature_riscv_op(...)

// 4. Use in lowering
BE(c)->feature_op(c, ...);
```

**Benefit**: All C99/C11/C2X features become ISA-independent and cross-platform.

---

## Additional Considerations

### Linker Support

| Feature | Backend | Notes |
|---------|---------|-------|
| **Multi-file compilation** | `link_backend` | Object files, symbol tables |
| **Static libraries** | `link_backend` | `.a` file extraction |
| **Dynamic libraries** | `link_backend` | `.so`/`.dll` loading |
| **Symbol resolution** | `link_backend` | dlsym, symbol tables |
| **Relocations** | `link_backend` | R_X86_64_*, R_AARCH64_* |

### Floating Point & SIMD

| Feature | Backend | Platform-Specific? |
|---------|---------|-------------------|
| **`float`/`double`** | `isa_backend` | Yes (XMM/SIMD regs) |
| **`_Float16`** | `isa_backend` | Yes (half precision) |
| **SSE/AVX** | `isa_backend` | x86 only |
| **NEON** | `isa_backend` | ARM only |
| **RISC-V Vector** | `isa_backend` | RVV extension |

### Runtime Features

| Feature | Backend | Notes |
|---------|---------|-------|
| **`setjmp`/`longjmp`** | `isa_backend` | Stack unwinding |
| **Signal handling** | `isa_backend` | Signal frames |
| **Exception handling** | `abi_backend` | DWARF CFI, SEH |
| **Stack protection** | `isa_backend` | Stack canaries |

### Debug & Optimization

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **DWARF debug info** | `isa_backend` | .debug_* sections |
| **PDB debug info** | `isa_backend` | Windows only |
| **IR optimization** | Compiler | Constant folding, DCE |
| **Register allocation** | Compiler | Linear scan, graph coloring |
| **Inlining** | Compiler | Function inlining |

### Build & Distribution

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **AOT compilation** | Compiler | `.s`/`.o` output |
| **JIT compilation** | Compiler | Current mode |
| **Cross-compilation** | Compiler | Target different arch |
| **Standard library** | Integration | libc headers |
| **Preprocessor** | Preprocessor | Full C99 preprocessor |

### Testing & Quality

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **Test suite** | Tests | 100+ tests |
| **Fuzzing** | Testing | libFuzzer, AFL |
| **Static analysis** | Tools | clang-tidy, scan-build |
| **CI/CD** | Infrastructure | GitHub Actions |
| **Documentation** | Docs | User + API docs |

### Error Handling

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **Error recovery** | Parser | Continue after errors |
| **Diagnostic messages** | `diag_backend` | Errors, warnings |
| **Source location** | Parser | Line/column tracking |
| **Note/remarks** | `diag_backend` | Help messages |

### Type System

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **Full type checking** | Semantic | C99/C11 rules |
| **Type inference** | Semantic | `auto` (C23) |
| **Generic types** | Preprocessor | `_Generic` |
| **Variadic functions** | Semantic | `...` support |

### Performance

| Feature | Implementation | Notes |
|---------|----------------|-------|
| **IR optimization** | Compiler | DCE, constant folding |
| **Register allocation** | Compiler | Linear scan |
| **Instruction selection** | `isa_backend` | Optimal instructions |
| **Peephole optimization** | `isa_backend` | Instruction patterns |

---

## Final Recommendations

### Phase 1: Core (C99/C11 Conformance)
1. ✅ TLS (`_Thread_local`) - In progress
2. Atomic (`_Atomic`) - Next
3. ABI (`_Alignas`, `alignof`) - Next
4. Inline semantics - Next
5. Full floating point - Ongoing

### Phase 2: Build System
1. Linker backend - Multi-file compilation
2. AOT compilation - `.s`/`.o` output
3. Preprocessor - Full C99 preprocessor
4. Standard library - libc integration

### Phase 3: Production Features
1. Debug info - DWARF/PDB
2. Optimization passes - IR optimization
3. Error recovery - Continue after errors
4. Cross-compilation - Target different arch

### Phase 4: Quality
1. Test suite - 100+ comprehensive tests
2. Fuzzing - libFuzzer/AFL
3. Documentation - User + API docs
4. CI/CD - GitHub Actions

**Goal**: Full C11 compiler with cross-platform support, production-quality code generation.
