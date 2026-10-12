# Phase 3: Large Struct Returns >16 bytes (sret) - Revised Task List

## Objective
Implement struct return via hidden pointer (sret) for structs >16 bytes per System V AMD64 ABI Section 3.2.6.

## Current State (Verified)
- `ret_struct_sz` already exists on `ZeroCCFunction` (type_system.h:111)
- Return lowering at `ast_lowering_stmt.c:168` (≤8 byte and 9-16 byte cases)
- Tools exist: `sret_verifier.py`, `struct_abi_verifier.py`

## Required State
```asm
make_big20:
    movq %rdi, %rax          # Save hidden pointer (RDI)
    movl %esi, (%rax)        # Store 'a' to [RDI]
    movl %edx, 4(%rax)       # Store 'b' to [RDI+4]
    ...
    ret                      # Return RDI in RAX
```

## Revised Task List

### 0. Test First (NEW - Start Here!)
- [ ] **0.1** Create failing test cases (20, 24, 32 byte structs) - **DO THIS FIRST**
  - Test 20-byte struct return
  - Test 24-byte struct return  
  - Test 32-byte struct return
  - Test edge case: 17-byte struct (just over threshold)
- [ ] **0.2** Verify tests fail with current implementation
- [ ] **0.3** Run `sret_verifier.py --verify-all` to document failures

### 1. Detection (Simplified)
- [ ] **1.1** Derive `is_sret` from `ret_struct_sz > 16` (no new flag needed)
- [ ] **1.2** Add sret check at `ast_lowering_stmt.c:168` (after existing ≤8 and 9-16 cases)
- [ ] **1.3** Track sret pointer in `LoweringState` (reuse existing state)

### 2. Callee Implementation (Corrected)
- [ ] **2.1** Save RDI as hidden pointer at function entry (before arg processing)
- [ ] **2.2** Write struct fields to [RDI+offset] instead of local stack
- [ ] **2.3** Return RDI in RAX at function exit
- [ ] **2.4** Keep local stack for other uses (don't remove - just don't use for return)

### 3. Caller Implementation (Corrected)
- [ ] **3.1** Allocate buffer on stack for return struct
- [ ] **3.2** Pass buffer pointer in RDI **before** other arguments
- [ ] **3.3** **MODIFY REGISTER ASSIGNMENT**: Start int arg index at 1 (not 0) when sret
  - NOT a separate shuffle pass - change the register assignment logic
  - With sret: only 5 integer args in registers (RSI-R9), rest on stack
  - Stack args also shift: 6th+ args on stack (not 7th+)
- [ ] **3.4** Save return pointer from RAX after call
- [ ] **3.5** Read struct from buffer after call

### 4. Edge Cases (NEW - Missing from original)
- [ ] **4.1** Calls that discard result: `make(1).a`, `f(make(1))`
  - Still need temporary buffer even if result unused
- [ ] **4.2** Calls where result already exists: `r = make(1)`
  - Pass address of existing `r` as hidden pointer
- [ ] **4.3** Function pointers / indirect calls
  - Same sret handling as direct calls
- [ ] **4.4** Tail returns: `return other_call();`
  - Pass same sret pointer through to nested call

### 5. Backend Implementation (Scoped)
- [ ] **5.1** **x86-64 JIT** (`src/zero_cc/isa/x86_64.c`) - **Priority**
- [ ] **5.2** **x86-64 AOT** (`src/zero_cc/isa/x86_64_asm.c`) - **Priority**
- [ ] **5.3** AArch64 backend (x8 is sret register, not first arg) - **After x86-64**
- [ ] **5.4** RISC-V backend (a0 is sret register) - **After x86-64**

### 6. Testing & Verification (Improved)
- [ ] **6.1** Run `sret_verifier.py --verify-all` (should pass after fix)
- [ ] **6.2** Run `struct_abi_verifier.py` for ABI compliance
- [ ] **6.3** **Link with GCC** (better than assembly comparison):
  - Your callee called from GCC code
  - GCC callee called from your code
  - Both directions must work
- [ ] **6.4** Update test suite (add >16 byte struct return tests)
- [ ] **6.5** Run `make test` (maintain 128+ tests passing)

### 7. Documentation
- [ ] **7.1** Update CHANGELOG.md with Phase 3 completion
- [ ] **7.2** Update ZEROCC_Development_Plan.md
- [ ] **7.3** Document sret implementation in code comments

## Files to Modify

### Core Compiler
- `src/zero_cc/parser/ast_lowering_stmt.c` - Struct return lowering (line 168)
- `src/zero_cc/jit/compile.h` - JIT compile path (lines 221, 241)
- `src/zero_cc/types/type_system.h` - `ret_struct_sz` already exists (line 111)

### Backend
- `src/zero_cc/isa/x86_64.c` - JIT code generation
- `src/zero_cc/isa/x86_64_asm.c` - AOT assembly emission

### Test Suite
- `test/abi/` - Add >16 byte struct return tests
- `test/jit/` - Add >16 byte struct return tests

## Verification Commands

```bash
# 1. Run sret verifier (should fail before, pass after)
python3 tools/sret_verifier.py --verify-all

# 2. Run struct ABI verifier
python3 tools/struct_abi_verifier.py --test test/abi/struct_return_20byte.c

# 3. Run test suite
make test

# 4. Link with GCC (better than assembly diff)
gcc -c test_gcc.c -o test_gcc.o
./build/bin/zerocc test_zerocc.c -c -o test_zerocc.o
gcc test_gcc.o test_zerocc.o -o test_combined
./test_combined  # Should work both ways
```

## Success Criteria

1. ✅ All >16 byte struct returns use hidden pointer in RDI
2. ✅ Callee writes struct to caller-provided buffer
3. ✅ Callee returns caller buffer pointer in RAX
4. ✅ Caller allocates buffer and passes in RDI
5. ✅ Caller shifts arg register assignment (index starts at 1)
6. ✅ All 128+ tests pass
7. ✅ Links and runs with GCC-compiled code (both directions)
8. ✅ sret_verifier.py reports all checks passing

## Scope & Dependencies

- **Phase 3a**: x86-64 only (JIT + AOT) - **2 days**
- **Phase 3b**: AArch64 (x8 sret register) - **0.5 days**
- **Phase 3c**: RISC-V (a0 sret register) - **0.5 days**
- **__int128**: Separate phase (not implemented yet)

## Key Corrections from Original Plan

1. **Test first** - Create failing tests before implementation
2. **Register assignment** - Modify index logic, not separate shuffle pass
3. **Local stack** - Keep for other uses, just write to [RDI] for return
4. **No new flag** - Derive `is_sret` from `ret_struct_sz > 16`
5. **Edge cases** - Discarded results, function pointers, tail returns
6. **GCC linking** - Better verification than assembly comparison
7. **Scoped backends** - x86-64 first, then others
