# IR Debug Tools

This document describes the Python tools created to debug the IR generation bug where accumulator values are stored to memory but never reloaded before use.

## Bug Summary

**Symptom**: `acc = 42; acc = acc + abs_val(10);` returns wrong value (20 instead of 52)

**Root Cause**: The accumulator value (42) is stored to memory but never reloaded before the addition. Instead, a different value (flags) is used.

**IR Pattern**:
```
1: MOV_IMM r16, 42        ; acc = 42
2: MOV_MEM [sp-...], r16  ; store acc
...
9: ADD r21, r17, r20      ; BUG: r17 (flags) used instead of reloading r16
```

## Tools Created

### 1. ir_accumulator_bug_detector.py

**Purpose**: Specifically detects the accumulator bug pattern

**Usage**:
```bash
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt --verbose
```

**Output**:
```
Found 4 accumulator bug(s):

Bug #1:
  Function: main
  Issue: Accumulator r16 (value 42) stored at instr 2 but r17 used in ADD at instr 9 instead of reloading
```

**Detection Logic**:
1. Find MOV_IMM instructions (accumulator candidates)
2. Track which candidates are stored to memory
3. Check for arithmetic operations after stores
4. Report if accumulator temp is not used in the arithmetic

### 2. ir_load_store_analyzer.py

**Purpose**: General analysis of load/store patterns

**Usage**:
```bash
python3 tools/ir_load_store_analyzer.py --ir ir_dump.txt
python3 tools/ir_load_store_analyzer.py --ir ir_dump.txt --verbose
```

**Detection Logic**:
1. Track memory stores and kills
2. Track loads and restores
3. Detect uses of killed temps without reload
4. Report missing load patterns

### 3. ir_value_flow_analyzer.py

**Purpose**: Track value flow through IR instructions

**Usage**:
```bash
python3 tools/ir_value_flow_analyzer.py --ir ir_dump.txt
python3 tools/ir_value_flow_analyzer.py --ir ir_dump.txt --trace r16
```

**Features**:
- Tracks where each temp's value resides (reg, mem, imm)
- Detects uses of memory values without reload
- Generates value flow traces for specific temps

### 4. ir_fix_verifier.py

**Purpose**: Verify that IR generation bugs are fixed

**Usage**:
```bash
python3 tools/ir_fix_verifier.py --ir ir_dump.txt --check-memory
python3 tools/ir_fix_verifier.py --ir ir_dump.txt --compare ir_dump_fixed.txt
```

**Checks**:
- All memory stores have corresponding loads before use
- Values are not used after being stored without reload
- Stack slot management is correct

## How to Use These Tools

### 1. Generate IR Dump

```bash
./build/bin/zerocc -c test.c -o test.o --target aarch64 -dump-ir 2> ir_dump.txt
```

### 2. Run Accumulator Bug Detector

```bash
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt --verbose
```

### 3. Analyze Results

If bugs are detected:
1. Check the IR dump for missing MOV instructions
2. Look at AST lowering code in `src/zero_cc/parser/ast_lowering_expr.c`
3. Verify expression evaluation preserves accumulator values

### 4. Verify Fix

After fixing the code:
```bash
./build/bin/zerocc -c test.c -o test.o --target aarch64 -dump-ir 2> ir_dump_fixed.txt
python3 tools/ir_fix_verifier.py --ir ir_dump_fixed.txt --check-memory
```

## Example Debug Session

```bash
# 1. Generate IR for buggy code
./build/bin/zerocc -c /tmp/test_accum.c -o /tmp/test_accum.o --target aarch64 -dump-ir 2> ir_dump.txt

# 2. Run detector
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt --verbose

# 3. Output shows:
#    Bug #1: Accumulator r16 (value 42) stored at instr 2 but r17 used in ADD at instr 9

# 4. Check IR dump
cat ir_dump.txt | grep -A 20 "function 'main'"

# 5. Look for missing load between instr 2 and instr 9

# 6. Fix AST lowering code to add missing load

# 7. Verify fix
./build/bin/zerocc -c /tmp/test_accum.c -o /tmp/test_accum.o --target aarch64 -dump-ir 2> ir_dump_fixed.txt
python3 tools/ir_accumulator_bug_detector.py --ir ir_dump_fixed.txt
# Should show: No accumulator bugs detected!
```

## Integration with CI/CD

These tools can be integrated into CI/CD to catch IR generation bugs:

```yaml
# Example GitHub Actions step
- name: Check IR generation
  run: |
    ./build/bin/zerocc -c test/test_accum.c -o test.o --target aarch64 -dump-ir 2> ir_dump.txt
    python3 tools/ir_accumulator_bug_detector.py --ir ir_dump.txt
```

## Future Improvements

1. **Automated Fix Suggestion**: Tool could suggest the exact MOV instruction to add
2. **AST Lowering Coverage**: Track which AST nodes generate correct IR
3. **Regression Tests**: Add test cases for known bug patterns
4. **IR Comparison**: Compare IR before/after optimization passes

## Related Documentation

- `docs/CALLER_SAVED_REGISTER_BUG.md` - Register allocation bug analysis
- `docs/REGALLOC_FIX_STATUS.md` - Register allocator fix status
- `docs/DEBUG_SUMMARY.md` - General debugging summary
