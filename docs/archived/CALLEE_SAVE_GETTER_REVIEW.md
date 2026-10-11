# Review: Adding get_callee_save_info() to ISACallingConvention

## Current Implementation

### Data Structure (isa_backend.h)
```c
typedef struct {
    const int *int_arg_regs;
    int int_arg_reg_count;
    const int *fp_arg_regs;
    int fp_arg_reg_count;
    const int *callee_saved_gpr;      // Array of physical register numbers
    int callee_saved_gpr_count;
    const int32_t *callee_save_slot_offsets; // Indexed by array position
} ISACallingConvention;
```

### Usage Pattern (ast_lowering_mc.c)
```c
case OP_CALLEE_SAVE_STORES:
    if (BE(c)->conv && BE(c)->conv->callee_saved_gpr && BE(c)->emit_store) {
        uint32_t mask = (uint32_t)instr->imm64;
        const int *callee_regs = BE(c)->conv->callee_saved_gpr;
        const int32_t *slot_offsets = BE(c)->conv->callee_save_slot_offsets;
        int count = BE(c)->conv->callee_saved_gpr_count;
        
        for (int i = 0; i < count; i++) {
            int reg = callee_regs[i];
            if ((mask >> reg) & 1u) {
                int32_t slot = slot_offsets ? slot_offsets[i] : (8 + (i * 8));
                BE(c)->emit_store(c, reg, BE(c)->fp_reg, -slot);
            }
        }
    }
    break;
```

## Analysis: Does get_callee_save_info() Make Sense?

### Arguments FOR Adding a Getter Function

1. **Encapsulation**: Hides the internal table structure
2. **Safety**: Can add bounds checking
3. **Flexibility**: Can change internal representation without breaking callers
4. **Consistency**: Similar to how other backends use function pointers

### Arguments AGAINST Adding a Getter Function

1. **Redundant**: The current table-based approach is already clean and simple
2. **Performance**: Function call overhead (though likely negligible)
3. **Complexity**: Adds another layer without significant benefit
4. **Existing Pattern**: Other calling convention info (arg_regs, etc.) also use direct table access

### Current Design Assessment

The current design follows a **data-driven approach**:
- Tables are defined once per ISA
- Multiple code paths can access the same data
- No function call overhead
- Easy to understand and debug

This is appropriate because:
1. **Callee-save info is static** - doesn't change at runtime
2. **Multiple access patterns** - different code paths need different info
3. **Simple indexing** - array position → slot offset is straightforward

## Recommendation: DON'T ADD get_callee_save_info()

**Reasoning**: The current table-based design is appropriate for callee-save register information because:

1. **Static Data**: Callee-save registers don't change at runtime
2. **Multiple Consumers**: Different code paths need different aspects (register number, slot offset, count)
3. **Simple Access**: Direct array indexing is clear and efficient
4. **Consistency**: Matches the pattern used for argument registers

### Alternative: Add Helper Macros Instead

If we want better encapsulation without the overhead, consider **helper macros**:

```c
/* In isa_backend.h */
#define CALLEE_SAVE_GET_REG(conv, idx) \
    ((conv)->callee_saved_gpr[(idx)])

#define CALLEE_SAVE_GET_SLOT(conv, idx) \
    ((conv)->callee_save_slot_offsets ? \
     (conv)->callee_save_slot_offsets[(idx)] : (8 + ((idx) * 8)))

#define CALLEE_SAVE_COUNT(conv) \
    ((conv)->callee_saved_gpr_count)
```

**Benefits**:
- Better encapsulation (single point of change)
- Default slot offset calculation centralized
- No function call overhead
- Easier to maintain

## Python Tools Available for Verification

### Existing Tools (✓ = Can verify callee-save, ✗ = Cannot)

| Tool | Can Verify Callee-Save | Purpose |
|------|----------------------|---------|
| `callee_saved_analyzer.py` | ✓ | Analyzes callee-save handling across ISAs |
| `verify_callee_save_fix.py` | ✓ | Verifies callee-save preservation |
| `prologue_verifier.py` | ✓ | Verifies prologue bytes (includes callee-save) |
| `prologue_validator.py` | ✗ | Validates prologue structure only |
| `stack_alignment_checker.py` | ✗ | Checks stack alignment only |
| `frame_layout_analyzer.py` | ✓ | Analyzes frame layout including callee-save slots |
| `verify_aarch64_callee_saved.py` | ✓ | AArch64-specific callee-save verification |
| `refactor_callee_saved.py` | ✓ | Callee-save refactoring verification |

### Tool Coverage Assessment

**Good Coverage**: We have 7 tools that can verify callee-save register handling:
- `callee_saved_analyzer.py` - General analysis
- `verify_callee_save_fix.py` - End-to-end verification
- `prologue_verifier.py` - Byte-level verification
- `frame_layout_analyzer.py` - Frame layout analysis
- `verify_aarch64_callee_saved.py` - AArch64-specific
- `refactor_callee_saved.py` - Refactoring verification
- `find_callee_save_calls.py` - Call site analysis

**Recommendation**: Run these tools to verify current implementation:

```bash
# General analysis
python3 tools/callee_saved_analyzer.py --check-isas
python3 tools/callee_saved_analyzer.py --verify-backend

# End-to-end verification
python3 tools/verify_callee_save_fix.py --full-test

# Byte-level verification (for each ISA)
python3 tools/prologue_verifier.py --isa x86_64 /path/to/object.o
python3 tools/prologue_verifier.py --isa aarch64 /path/to/object.o

# Frame layout analysis
python3 tools/frame_layout_analyzer.py --asm-file test.s
```

## Conclusion

**Decision**: Do NOT add `get_callee_save_info()` function.

**Rationale**:
1. Current table-based design is appropriate for static calling convention data
2. Multiple code paths need different aspects of the data
3. Direct table access is efficient and clear
4. We have comprehensive Python tools for verification

**Alternative**: Consider adding helper macros for better encapsulation if needed.

**Verification**: Use existing Python tools (`callee_saved_analyzer.py`, `verify_callee_save_fix.py`, `prologue_verifier.py`) to verify current implementation is correct.
