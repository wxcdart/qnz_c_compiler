# Flag DCE Optimization - Complete

## Summary

Flag DCE (Dead Code Elimination) optimization has been successfully implemented. The pass removes CMP instructions whose flags are never used.

## Implementation

### Algorithm

```c
void opt_flag_dce(ZeroCCCompiler *c) {
    for (int i = 0; i < instr_count; i++) {
        Instr *instr = ir_get(i);
        
        // Only analyze flag-setting instructions
        if (instr->op != OP_CMP && instr->op != OP_CMP_ZERO) {
            continue;
        }
        
        // Check if flags are used by subsequent instructions
        bool flags_used = false;
        for (int j = i + 1; j < instr_count; j++) {
            Instr *next = ir_get(j);
            
            // Stop at control flow boundary
            if (is_control_flow(next->op)) {
                break;
            }
            
            // Check if this instruction reads flags
            if (next->flags_in == REG_FLAGS) {
                flags_used = true;
                break;
            }
        }
        
        // Mark as dead if flags never used
        if (!flags_used) {
            instr->op = OP_DEAD;
        }
    }
}
```

### Optimization Pipeline

```
constant_fold -> flag_dce -> dce -> cse -> licm -> ...
```

The flag_dce pass runs:
- **After** constant folding (to catch dead CMPs from folded comparisons)
- **Before** general DCE (which removes OP_DEAD instructions)

## Files Modified

```
src/zero_cc/parser/opt_dce.c      (+62 lines: opt_flag_dce implementation)
src/zero_cc/parser/opt_dce.h      (+7 lines: declaration)
src/zero_cc/parser/opt_pass.c     (+4 lines: pipeline integration)
```

Total: ~73 lines added

## Testing

All 118 JIT tests pass:
```
Results: 118/118 passed, 0 failed in 0.30s
```

## Example

### Before Flag DCE
```
CMP a, b      // flags_out = REG_FLAGS
ADD x, y      // flags_in = 0xFF (doesn't use flags)
```

### After Flag DCE
```
ADD x, y
```

The CMP is removed because no instruction reads its flags.

## Future Work

### Flag CSE (Common Subexpression Elimination)

The next optimization would be Flag CSE, which shares CMP results across multiple users:

```
// Before:
CMP a, b
COND_SET x, EQ
COND_SET y, NE

// After (with Flag CSE):
CMP a, b
COND_SET x, EQ
COND_SET y, NE  // Reuses flags from first CMP
```

This is more complex and can be implemented later when needed.

## Conclusion

Flag DCE is now part of the optimization pipeline. It provides:
- ✓ Automatic removal of dead CMP instructions
- ✓ Low risk (only removes dead code)
- ✓ Uses explicit flag dependency tracking
- ✓ All tests pass
