# ZeroCC Stack Allocation & VLA Support Design

## Overview

This document outlines the design for fixing large stack allocation failures and implementing proper VLA (Variable Length Array) support in ZeroCC.

## Current Architecture Analysis

### What ZeroCC Already Has

| Component | Status | Stack-Relevant |
|-----------|--------|----------------|
| **Prologue/Epilogue** | ✅ Complete | Calculates frame size |
| **Frame size calc** | ✅ Complete | Locals + spills + callee-saves |
| **VLA opcode** | ✅ OP_VLA_ALLOC | Exists but may be incomplete |
| **Stack alignment** | ✅ 16-byte | Maintained by prologue |
| **Minimum frame** | ✅ 32 bytes | Ensures proper alignment |

### What's Missing

| Component | Needed For |
|-----------|------------|
| Large frame support | Frames > 32KB currently fail |
| VLA runtime allocation | Dynamic stack size at runtime |
| Stack canary | Security (optional) |
| Red zone handling | x86-64 128-byte red zone |
| Stack overflow detection | Guard pages |

## Root Cause Analysis

### memory_heavy.c Failure

```c
int main() {
    int arr[1000];        // 4KB - OK
    int matrix[100][100]; // 40KB - FAILS!
    // ...
}
```

**GCC Assembly:**
```asm
leaq    -40960(%rsp), %r11   ; Allocate 40KB stack frame
subq    $4096, %rsp
orq     $0, (%rsp)           ; Stack probe (touch pages)
```

**ZeroCC (likely):**
```asm
; Current implementation may:
; 1. Not support frames > 32KB
; 2. Not probe stack pages
; 3. Not handle VLA properly
```

## Design Principles

### 1. Incremental Stack Growth

Large allocations should probe stack pages to avoid segfaults:
```c
// GCC does this for large allocations:
subq $4096, %rsp
orq $0, (%rsp)  // Touch page to trigger guard page if needed
```

### 2. VLA Runtime Allocation

Variable-length arrays need runtime stack pointer adjustment:
```c
void foo(int n) {
    int arr[n];  // Stack size depends on runtime value
}
```

### 3. Compatibility with Existing Code

- Keep prologue/epilogue for fixed-size frames
- Add separate path for VLA/large allocations
- Maintain 16-byte stack alignment

## Stack Frame Design

### Frame Size Limits

| Size | Handling |
|------|----------|
| 0-32KB | Direct allocation in prologue |
| 32KB-1MB | Large allocation with page probing |
| >1MB | Error (stack overflow) |
| VLA | Runtime allocation |

### Stack Layout

```
Higher addresses
┌─────────────────┐
│ Function args   │
├─────────────────┤
│ Return address  │
├─────────────────┤
│ Saved RBP       │
├─────────────────┤
│ Callee-saved    │
│ (rbx, rbp, r12- │
│  r15 if used)   │
├─────────────────┤
│ Fixed locals    │ ← Frame base (RBP)
├─────────────────┤
│ VLA space       │ ← Optional (runtime size)
├─────────────────┤
│ Spill slots     │
├─────────────────┤
│ Red zone        │ (128 bytes, x86-64)
└─────────────────┘
Lower addresses
```

## VLA Implementation

### VLA Opcode Extension

Extend `OP_VLA_ALLOC` in `ir.h`:

```c
OP_VLA_ALLOC,      /* Allocate VLA: temp_dest = base address, imm = size in bytes */
OP_VLA_FREE,       /* Deallocate VLA: restore stack pointer */
```

### VLA Lowering

Update `ast_lowering_expr.h`:

```c
static int lower_vla(ZeroCCCompiler *c, SemExpr *expr) {
    // 1. Calculate size at runtime
    int size_reg = lower_expr(c, expr->size_expr);
    
    // 2. Align size to 16 bytes
    ir_emit(OP_AND_IMM, size_reg, size_reg, 0xFFFFFFF0);
    
    // 3. Save current RSP
    int saved_rsp = ir_emit_temp();
    ir_emit(OP_MOV_RSP, saved_rsp, 0);
    
    // 4. Allocate: RSP -= size
    ir_emit(OP_SUB_RSP, 0, size_reg);
    
    // 5. Return base address (current RSP)
    return saved_rsp;  // Or new RSP
}
```

### VLA Runtime Code

```c
// VLA allocation (runtime):
push    rbp             ; Save old RBP
mov     rbp, rsp        ; Save stack pointer
mov     rax, [size]     ; Load VLA size
and     rax, 0xFFFFFFF0 ; Align to 16 bytes
sub     rsp, rax        ; Allocate
; ... use VLA at [rsp] ...
mov     rsp, rbp        ; Deallocate
pop     rbp             ; Restore
ret
```

## Large Frame Support

### Page Probing

For frames > 32KB, probe stack pages:

```c
static void emit_large_frame_prologue(ZeroCCCompiler *c, int frame_size) {
    // 1. Standard prologue
    ir_emit(OP_PUSH_RBP);
    ir_emit(OP_MOV_RBP_RSP);
    
    // 2. Calculate number of pages to probe
    int pages = (frame_size + 4095) / 4096;
    
    // 3. Probe each page (touch memory to trigger guard page)
    int temp = ir_emit_temp();
    ir_emit(OP_MOV_IMM, temp, frame_size - 4095);
    
    for (int i = 0; i < pages; i++) {
        int offset = frame_size - 4095 - (i * 4096);
        ir_emit(OP_STORE_BYTE, 0, temp, offset);  // Touch page
    }
    
    // 4. Final stack adjustment
    ir_emit(OP_SUB_RSP, 0, frame_size);
}
```

### x86-64 Assembly

```asm
; Large frame prologue (40KB):
push    rbp
mov     rbp, rsp
mov     rax, 36864      ; frame_size - 4095
mov     byte ptr [rsp + rax], 0  ; Probe page 1
sub     rsp, 4096
mov     byte ptr [rsp + rax - 4096], 0  ; Probe page 2
; ... repeat for all pages ...
sub     rsp, 4096       ; Final allocation
```

## Stack Canary (Optional)

### Stack Canary Design

Add optional stack protection:

```c
// Stack canary in TLS (Thread Local Storage)
// Placed at function entry, checked before return

static void emit_stack_canary(ZeroCCCompiler *c) {
    // 1. Load canary from TLS (%fs:40 on x86-64)
    int canary_reg = ir_emit_temp();
    ir_emit(OP_LOAD_TLS, canary_reg, 40);  // %fs:40
    
    // 2. Store canary at [RBP - 8]
    ir_emit(OP_STORE_64, 0, canary_reg, -8);
    
    // 3. Check canary before epilogue
    ir_emit(OP_LOAD_64, canary_reg, 0, -8);
    ir_emit(OP_XOR, canary_reg, canary_reg, canary_reg);
    ir_emit(OP_JNE, canary_reg, 0, epilogue_label);
    ir_emit(OP_CALL, __stack_chk_fail);
}
```

### x86-64 Assembly

```asm
; Stack canary setup:
mov     rax, qword ptr [fs:40]  ; Load canary
mov     qword ptr [rbp - 8], rax ; Store on stack

; Stack canary check (before return):
mov     rax, qword ptr [rbp - 8] ; Load canary
xor     rax, qword ptr [fs:40]   ; Compare
je      .ok
call    __stack_chk_fail         ; Fail if mismatch
.ok:
leave
ret
```

## Implementation Phases

### Phase 1: Large Frame Support (2 days)

**Task 74: Stack Frame Expansion**

- [ ] Extend frame size calculation to support 1MB
- [ ] Add page probing for frames > 32KB
- [ ] Update prologue emission for large frames
- [ ] Test with 40KB allocation (memory_heavy.c)

### Phase 2: VLA Support (3 days)

**Task 75: VLA Runtime Allocation**

- [ ] Extend `OP_VLA_ALLOC` with runtime size
- [ ] Implement VLA lowering in `ast_lowering_expr.h`
- [ ] Add VLA deallocation (`OP_VLA_FREE`)
- [ ] Handle VLA in register allocation
- [ ] Test with VLA examples

### Phase 3: Stack Canary (1 day)

**Task 76: Stack Canary Support**

- [ ] Add TLS access for canary value
- [ ] Implement canary setup in prologue
- [ ] Implement canary check in epilogue
- [ ] Link with `__stack_chk_fail`
- [ ] Optional: enable via `-fstack-protector`

### Phase 4: Testing (1 day)

**Task 77: Large Array Testing**

- [ ] Test memory_heavy.c (40KB array)
- [ ] Test with various array sizes (1KB, 16KB, 64KB, 256KB)
- [ ] Test VLA with runtime sizes
- [ ] Test stack canary with buffer overflow
- [ ] Verify all 118 existing tests still pass

## Stack Allocation Flow

### Fixed-Size Arrays

```
compile_file()
  └─> lower_function()
      └─> calculate_frame_size()  ← Static size known
          └─> emit_prologue()      ← Allocate in prologue
              └─> [rsp - frame_size]
```

### Variable-Length Arrays

```
compile_file()
  └─> lower_function()
      └─> lower_vla_alloc()        ← Runtime size
          └─> emit_vla_prologue()  ← Allocate at runtime
              └─> [rsp - size] (dynamic)
```

### Large Fixed Arrays

```
compile_file()
  └─> lower_function()
      └─> calculate_frame_size()  ← Size > 32KB
          └─> emit_large_prologue()
              └─> Probe pages
              └─> [rsp - frame_size]
```

## Edge Cases

### 1. Nested VLAs

```c
void foo(int n, int m) {
    int arr1[n];      // VLA 1
    int arr2[m];      // VLA 2
    // ...
}
```

**Solution:** Track VLA stack depth, deallocate in reverse order.

### 2. VLA in Loops

```c
for (int i = 0; i < n; i++) {
    int arr[i];  // VLA allocated/deallocated each iteration
}
```

**Solution:** Allocate at loop entry, deallocate at loop exit.

### 3. Stack Overflow

```c
void foo() {
    int arr[10*1024*1024];  // 10MB - too large!
}
```

**Solution:** Error if frame_size > 1MB.

### 4. Red Zone

x86-64 ABI reserves 128 bytes below RSP for leaf functions.

**Solution:** Don't use red zone if frame_size > 0.

## Testing

### Test Cases

| Test | Size | Expected |
|------|------|----------|
| Small array | 1KB | Pass |
| Medium array | 16KB | Pass |
| Large array | 40KB | Pass (memory_heavy.c) |
| VLA | runtime | Pass |
| Nested VLA | runtime | Pass |
| Too large | >1MB | Error |

### Test File: memory_heavy.c

```c
int main() {
    int arr[1000];        // 4KB - should work
    int matrix[100][100]; // 40KB - currently fails
    
    // Initialize
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 100; j++) {
            matrix[i][j] = i * 100 + j;
        }
    }
    
    // Sum
    int sum = 0;
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 100; j++) {
            sum += matrix[i][j];
        }
    }
    
    return sum;  // Expected: 495000
}
```

## Integration with Existing Code

### Prologue Update

```c
// Current (ast_lowering.c):
static void emit_prologue(ZeroCCCompiler *c) {
    int frame_size = calculate_frame_size(c);
    BE(c)->emit_prologue(c, frame_size);
}

// Updated:
static void emit_prologue(ZeroCCCompiler *c) {
    int frame_size = calculate_frame_size(c);
    
    if (frame_size > 32768) {
        // Large frame: use page probing
        emit_large_frame_prologue(c, frame_size);
    } else {
        // Normal frame
        BE(c)->emit_prologue(c, frame_size);
    }
}
```

### Epilogue Update

```c
// Current:
static void emit_epilogue(ZeroCCCompiler *c) {
    BE(c)->emit_epilogue(c);
}

// Updated (with stack canary):
static void emit_epilogue(ZeroCCCompiler *c) {
    if (c->stack_canary_enabled) {
        emit_stack_canary_check(c);
    }
    BE(c)->emit_epilogue(c);
}
```

## Success Criteria

- [ ] memory_heavy.c passes (exit code 80)
- [ ] Support frames up to 1MB
- [ ] VLA allocation/deallocation works
- [ ] Stack canary optional (disabled by default)
- [ ] All 118 existing tests pass
- [ ] No stack overflow segfaults

## References

- x86-64 ABI Spec: Section 3.2.2 (Stack Frame)
- GCC VLA Implementation: https://gcc.gnu.org/onlinedocs/gcc/Variable-Length.html
- Stack Canaries: https://en.wikipedia.org/wiki/Buffer_overflow#Protection

---

*Last updated: 2024-10-05*
*Status: Design complete, ready for implementation*
*Target: Sprint 11 (Weeks 38-39)*
