# Phase 2B Tools Summary

## Tools Created

### 1. `tools/opcode_usage_analyzer.py`

**Purpose:** Find all SETcc opcode usages to plan refactoring

**Usage:**
```bash
python3 tools/opcode_usage_analyzer.py
```

**Output:**
- Lists all 25 SETcc opcode references
- Shows which files need to be modified
- Provides step-by-step refactoring plan

**Current Status:**
- 25 SETcc usages found
- All in IR/lowering layer (no backend usages)
- 7 unique opcodes: OP_TEST, OP_SETL, OP_SETLE, OP_SETG, OP_SETGE, OP_SET_E, OP_SET_NE

### 2. `tools/phase2b_verifier.py`

**Purpose:** Verify Phase 2B completion

**Usage:**
```bash
python3 tools/phase2b_verifier.py
```

**Exit codes:**
- `0` - Phase 2B complete (all checks pass)
- `1` - Phase 2B incomplete (some checks fail)

**Checks:**
1. SETcc opcodes removed from IR layer
2. OP_CMP_BRANCH opcode added to ir.h
3. emit_cmp_branch() added to isa_backend.h

**Current Status:** ✗ INCOMPLETE (as expected)

### 3. `tools/cmp_codegen_checker.py`

**Purpose:** Verify comparison codegen works after Phase 2B

**Usage:**
```bash
python3 tools/cmp_codegen_checker.py
```

**Output:**
- Compiles comparison test cases
- Analyzes assembly for cmp/test patterns
- Verifies all comparison operations compile

**Current Status:** Ready to use after Phase 2B implementation

## Phase 2B Checklist

Use these tools to track progress:

### Before Implementation
```bash
# 1. Document current state
python3 tools/opcode_usage_analyzer.py > /tmp/before_phase2b.txt

# 2. Verify incomplete
python3 tools/phase2b_verifier.py  # Should return 1
```

### After Implementation
```bash
# 1. Verify complete
python3 tools/phase2b_verifier.py  # Should return 0

# 2. Run tests
make test  # 118/118 must pass

# 3. Verify codegen
python3 tools/cmp_codegen_checker.py

# 4. Cross-ISA comparison
python3 tools/isa_diff.py test/abi/comparison.c --isas x86_64 aarch64
```

## Implementation Files

Based on `opcode_usage_analyzer.py` output:

### ir.h (7 opcodes)
- Lines 50-54: OP_TEST, OP_SETL, OP_SETLE, OP_SETG, OP_SETGE
- Lines 83-84: OP_SET_E, OP_SET_NE
- Lines 139-170: Name functions

### ast_lowering_mc.c (8 opcodes)
- Lines 147-171: Switch cases for all SETcc opcodes
- Line 468: OP_LNOT

### opt_dce.c (1 opcode)
- Line 74: OP_TEST in side effects list

## Refactoring Steps

1. **Add OP_CMP_BRANCH to ir.h**
   - New opcode enum
   - Condition code constants (CC_LT, CC_LE, etc.)
   - Helper function ir_emit_cmp_branch()

2. **Update isa_backend.h**
   - Add emit_cmp_branch() function pointer
   - Add condition code defines

3. **Implement backends**
   - x86_64.c: cmp + setcc
   - aarch64.c: cmp + cset
   - riscv.c: slt + branch

4. **Update IR lowering**
   - Replace OP_TEST usage
   - Replace OP_SETL/SETLE/SETG/SETGE usage
   - Replace OP_SET_E/SET_NE usage
   - Replace OP_LNOT usage

5. **Remove old opcodes**
   - Remove from ir.h
   - Remove switch cases from ast_lowering_mc.c
   - Remove from opt_dce.c

6. **Verify**
   - python3 tools/phase2b_verifier.py (should pass)
   - make test (118/118)
   - python3 tools/cmp_codegen_checker.py

## Timeline

| Step | Duration | Tool to Verify |
|------|----------|----------------|
| 1-2: Add IR/backend interface | 0.5 day | phase2b_verifier.py |
| 3: Implement backends | 1 day | - |
| 4: Update lowering | 1 day | phase2b_verifier.py |
| 5: Remove old opcodes | 0.5 day | phase2b_verifier.py |
| 6: Test and verify | 1 day | cmp_codegen_checker.py |

**Total: 4-5 days**

---

*Last updated: 2026-10-06*
