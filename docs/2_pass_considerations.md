# Two-Pass IR: Critical Considerations

## 1. Instruction Encoding

### Current Design (16 bytes)
```c
typedef struct {
    uint8_t  op;        // 1 byte
    int8_t   temp_dest; // 1 byte
    int8_t   temp_src1; // 1 byte
    int8_t   temp_src2; // 1 byte
    int32_t  imm;       // 4 bytes
    uint16_t label_idx; // 2 bytes
} Instr;  // Total: 12 bytes, but padded to 16
```

### Issues
- **Padding waste**: 4 bytes wasted on most instructions
- **Immediates too large**: Most immediates fit in int16 or int8
- **Labels only needed for jumps**: Wasted space for non-jumps

### Better Design (8 bytes)
```c
typedef struct {
    uint8_t  op;        // 1 byte
    uint8_t  flags;     // 1 byte (has_dest, has_src1, has_src2, has_imm, has_label)
    int16_t  temp_dest; // 2 bytes (-1 if none)
    int16_t  temp_src1; // 2 bytes (-1 if none)
    int16_t  temp_src2; // 2 bytes (-1 if none)
    int16_t  imm_low;   // 2 bytes (lower half of immediate)
    int16_t  imm_high;  // 2 bytes (upper half, or label_idx)
} Instr;  // Total: 12 bytes, still padded to 16 on some systems
```

### Best Design (Packed, 12 bytes)
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        // 0-255 opcodes
    uint8_t  temp_dest; // 0-127 temps (extend with flag if needed)
    uint8_t  temp_src1;
    uint8_t  temp_src2;
    int16_t  imm;       // -32768 to 32767 (enough for most immediates)
    uint16_t label_idx; // 0-65535 labels
} Instr;  // Total: 8 bytes!
```

**Impact:**
- 8 bytes × 65536 = **512KB** (vs 1MB)
- Better cache utilization
- Faster iteration

---

## 2. Label Handling

### Problem: Forward References
```c
// Jump to label that hasn't been seen yet
emit_instr(OP_JE, temp_cond, -1, -1, 0, label_foo);  // label_foo = ?
// ...
label_foo:
// ...
```

### Solution: Two-Phase Label Resolution

```c
typedef struct {
    uint16_t instr_idx;  // Which instruction needs patching
    uint16_t label_idx;  // Which label to patch
} LabelPatch;

static LabelPatch label_patches[4096];
static size_t patch_count = 0;

// Phase 1: Record label positions
for (size_t i = 0; i < instr_count; i++) {
    if (instrs[i].op == OP_LABEL) {
        label_positions[instrs[i].label_idx] = i;
    }
}

// Phase 2: Patch forward references
for (size_t i = 0; i < patch_count; i++) {
    LabelPatch *p = &label_patches[i];
    size_t target = label_positions[p->label_idx];
    // Patch instruction p->instr_idx with target offset
}
```

### Alternative: Relative Offsets
```c
// Store relative offset instead of label index
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;
    uint8_t  temp_src1;
    int16_t  offset;   // Relative offset for jumps
} Instr;

// Forward jump: offset = target - current
// Backward jump: offset = target - current (negative)
```

---

## 3. Spill Slot Allocation

### Problem: Where do spilled temps live?
```c
// After register allocation
for (size_t i = 0; i < temp_var_count; i++) {
    if (temp_vars[i].assigned_reg < 0) {
        // This temp is spilled - assign stack slot
        temp_vars[i].spill_slot = allocate_spill_slot();
    }
}
```

### Stack Frame Layout
```
[rbp]           <- frame pointer
[rbp + 8]       <- return address
[rbp - 8]       <- spilled temp 0
[rbp - 16]      <- spilled temp 1
[rbp - 24]      <- spilled temp 2
...
[rbp - N]       <- local variables
```

### Allocation Strategy
```c
static int next_spill_slot = 0;

static int allocate_spill_slot(void) {
    int slot = -8 - (next_spill_slot * 8);  // -8, -16, -24, ...
    next_spill_slot++;
    return slot;
}

static void reset_spill_slots(void) {
    next_spill_slot = 0;
}
```

---

## 4. Register Allocation Integration

### When Does It Run?
```
Pass 1: Lower AST → IR (track liveness)
        ↓
    Compute live ranges for all temps
        ↓
    Run linear-scan register allocation
        ↓
Pass 2: Emit machine code using allocated registers
```

### Data Flow
```c
// After Pass 1
for (size_t i = 0; i < temp_var_count; i++) {
    if (temp_vars[i].live_end == 0) {
        temp_vars[i].live_end = instr_count;  // End at last instruction
    }
    regalloc_add_interval(&c->regalloc, ...);
}
regalloc_allocate(&c->regalloc);

// Now temp_vars[i].assigned_reg is set
```

---

## 5. Multiple Functions

### Problem: Each function needs its own IR
```c
// Current: Global instrs[] array
static Instr instrs[MAX_INSTRS];  // Shared by all functions

// Issue: Can't compile multiple functions simultaneously
```

### Solution: Per-Function IR Buffer
```c
typedef struct {
    Instr *instrs;
    size_t capacity;
    size_t count;
} InstrBuffer;

// Compile function
void compile_function(ZeroCCCompiler *c, ZeroCCFunction *fn) {
    InstrBuffer buf = {NULL, 0, 0};
    instr_buffer_init(&buf, 1024);  // Start with 1K
    
    lower_expr_pass1(c, arena, fn->ast);
    optimize(&buf);
    regalloc_allocate(c, &buf);
    emit_machine_code(c, &buf);
    
    instr_buffer_free(&buf);
}
```

### Or: Reset Between Functions
```c
// Simpler: Just reset counter
void compile_function(ZeroCCCompiler *c, ZeroCCFunction *fn) {
    instr_count = 0;  // Reset for this function
    
    lower_expr_pass1(c, arena, fn->ast);
    // ...
}
```

**Trade-off:**
- Reset: Simpler, but only one function at a time
- Per-function buffer: More complex, supports parallel compilation

---

## 6. Debug Information

### Source Line Mapping
```c
typedef struct {
    size_t instr_idx;
    int line_number;
    const char *filename;
} DebugLocation;

static DebugLocation debug_locations[4096];
static size_t debug_count = 0;

// During Pass 1
void emit_instr_with_location(Instr instr, int line, const char *file) {
    emit_instr(instr);
    debug_locations[debug_count++] = (DebugLocation){
        .instr_idx = instr_count - 1,
        .line_number = line,
        .filename = file
    };
}
```

### Use Cases
- GDB debugging
- Error messages with source location
- Profiling

---

## 7. Type Information

### Why We Need It
```c
// Without types:
add rax, rcx  // Is this int + int or double + double?

// With types:
add rax, rcx       // int + int
addsd xmm0, xmm1   // double + double
```

### Solution: Type Annotations
```c
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;
    uint8_t  temp_src1;
    uint8_t  temp_src2;
    int16_t  imm;
    uint8_t  type;    // TYPE_INT, TYPE_FLOAT, TYPE_PTR
} Instr;
```

### Or: Separate IR for Different Types
```c
// Int operations
OP_ADD_RAX_RCX
OP_SUB_RAX_RCX

// Float operations  
OP_ADDSD_XMM0_XMM1
OP_SUBSD_XMM0_XMM1
```

---

## 8. Control Flow Graph (CFG)

### Why We Need It
- Loop detection (for unrolling)
- Dominator analysis (for optimization)
- Dead code elimination
- Register allocation (liveness analysis)

### CFG Structure
```c
typedef struct BasicBlock {
    size_t start_instr;
    size_t end_instr;
    struct BasicBlock *successors[2];  // up to 2 (branch)
    struct BasicBlock *predecessors[2];
    bool is_loop_header;
} BasicBlock;

static BasicBlock blocks[1024];
static size_t block_count = 0;
```

### Building CFG
```c
// After Pass 1, before optimization
void build_cfg(void) {
    block_count = 0;
    size_t current = 0;
    
    while (current < instr_count) {
        BasicBlock *bb = &blocks[block_count++];
        bb->start_instr = current;
        
        // Find branch/jump
        while (current < instr_count) {
            if (is_branch(instrs[current].op)) {
                bb->end_instr = current;
                // Set successors
                break;
            }
            current++;
        }
        current++;  // Skip branch instruction
    }
}
```

---

## 9. Stack Frame Layout

### Complete Frame Structure
```
Higher addresses
+------------------+
| Function args    |  <- [rbp + 8], [rbp + 16], ...
+------------------+
| Return address   |  <- [rbp + 0]
+------------------+
| Saved rbp        |  <- [rbp - 0]
+------------------+
| Spilled temps    |  <- [rbp - 8], [rbp - 16], ...
+------------------+
| Local vars       |  <- [rbp - N], ...
+------------------+
| Red zone         |  <- 128 bytes (System V ABI)
+------------------+
Lower addresses
```

### Frame Size Calculation
```c
size_t calculate_frame_size(void) {
    size_t spilled = next_spill_slot * 8;
    size_t locals = next_local_offset;
    size_t total = spilled + locals + 16;  // +16 for rbp/return
    
    // Align to 16 bytes
    return (total + 15) & ~15;
}
```

---

## 10. Edge Cases

### Very Large Functions (>64K instructions)
```c
if (instr_count >= MAX_INSTRS) {
    // Option 1: Error (fail fast)
    fprintf(stderr, "Function too large\n");
    exit(1);
    
    // Option 2: Switch to vector (dynamic)
    // Option 3: Split into multiple IR buffers
}
```

### Recursion
```c
// Each function call creates new IR buffer
// No shared state between recursive calls
// Safe as long as we reset between functions
```

### Inline Assembly
```c
// Need special instruction type
typedef struct {
    uint8_t  op;
    uint8_t  asm_code;  // INLINE_ASM
    char    *asm_text;  // Raw assembly
} Instr;
```

### Variadic Functions
```c
// Need to track register vs stack arguments
typedef struct {
    uint8_t  op;
    uint8_t  temp_dest;
    uint8_t  num_reg_args;  // How many args in registers
    uint8_t  num_stack_args;
} Instr;
```

---

## 11. Optimization Considerations

### Dead Code Elimination
```c
// Mark used temps
bool temp_is_used[256] = {false};
for (size_t i = 0; i < instr_count; i++) {
    if (instrs[i].temp_src1 >= 0) temp_is_used[instrs[i].temp_src1] = true;
    if (instrs[i].temp_src2 >= 0) temp_is_used[instrs[i].temp_src2] = true;
}

// Remove unused
size_t write = 0;
for (size_t read = 0; read < instr_count; read++) {
    if (temp_is_used[instrs[read].temp_dest]) {
        instrs[write++] = instrs[read];
    }
}
instr_count = write;
```

### Constant Propagation
```c
// Track constant values
int64_t const_value[256];
bool is_constant[256] = {false};

for (size_t i = 0; i < instr_count; i++) {
    Instr *instr = &instrs[i];
    
    if (instr->op == OP_MOV_IMM) {
        is_constant[instr->temp_dest] = true;
        const_value[instr->temp_dest] = instr->imm;
    }
    
    if (is_constant[instr->temp_src1] && is_constant[instr->temp_src2]) {
        // Fold constant operation
        int64_t result = compute(instr->op, 
                                  const_value[instr->temp_src1],
                                  const_value[instr->temp_src2]);
        instr->op = OP_MOV_IMM;
        instr->temp_src1 = -1;
        instr->temp_src2 = -1;
        instr->imm = result;
    }
}
```

---

## 12. Testing Strategy

### Unit Tests
- [ ] Individual opcodes emit correct bytes
- [ ] Register allocation assigns correct registers
- [ ] Spill code loads/stores correctly
- [ ] Labels patch correctly

### Integration Tests
- [ ] Simple arithmetic (+, -, *, /)
- [ ] Control flow (if, while, for)
- [ ] Function calls (with/without args)
- [ ] Recursive functions
- [ ] Large functions (stress test)

### Performance Tests
- [ ] Compilation speed (-O0 vs -O2)
- [ ] Generated code speed (benchmarks)
- [ ] Memory usage (IR buffer size)

---

## 13. Implementation Checklist

### Phase 1: Infrastructure
- [ ] Define Instr struct (8-12 bytes)
- [ ] Create instrs[] array (64K)
- [ ] Implement emit_instr(), reset_instrs()
- [ ] Add overflow check

### Phase 2: Pass 1 (Instruction Selection)
- [ ] Rename lower_expr → lower_expr_pass1
- [ ] Replace zero_cc_emit_* with emit_instr
- [ ] Track temp assignments
- [ ] Handle all AST node types

### Phase 3: Register Allocation
- [ ] Compute live ranges
- [ ] Run regalloc_allocate()
- [ ] Assign spill slots

### Phase 4: Pass 2 (Code Emission)
- [ ] Implement emit_machine_code()
- [ ] Handle register-based ops
- [ ] Generate spill code
- [ ] Patch labels

### Phase 5: Optimization
- [ ] Dead code elimination
- [ ] Constant propagation
- [ ] Loop unrolling (-O2)
- [ ] Strength reduction

### Phase 6: Testing
- [ ] All 71 existing tests pass
- [ ] Add optimization benchmarks
- [ ] Stress test large functions

---

## 14. Timeline Estimate

| Phase | Duration | Risk |
|-------|----------|------|
| Infrastructure | 1 day | Low |
| Pass 1 | 3-4 days | Medium |
| Register Alloc | 2 days | Low |
| Pass 2 | 4-5 days | High |
| Optimization | 5-7 days | Medium |
| Testing | 3-5 days | Medium |
| **Total** | **~3 weeks** | |

---

## 15. Risk Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| IR too large | Low | Medium | Increase MAX_INSTRS |
| Pass 2 bugs | High | High | Test each opcode |
| Spill code wrong | Medium | High | Force spills, test |
| Optimization breaks | Medium | Medium | Test -O0 first |
| Performance regression | Low | Medium | Benchmark early |

---

## Summary

**Key decisions:**
1. ✅ 64K instruction array (1MB or 512KB packed)
2. ✅ Per-function reset (not parallel compilation)
3. ✅ Two-pass with optimization between
4. ✅ Simple struct (8-12 bytes per instruction)

**Next step:**
Start with **Phase 1: Infrastructure** - define Instr struct and test basic emit/reset.
