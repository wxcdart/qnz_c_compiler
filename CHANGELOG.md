# Changelog

## [Unreleased] - Sprint 3: Two-Pass Code Generation

### Added

#### Phase 1: IR Infrastructure ✅
- Two-pass code generation architecture
- 64K instruction buffer (`src/zero_cc/parser/ir.h`)
- 8-byte packed instruction format
- Register allocation integration (linear-scan)
- Optimization level support (-O0, -O1, -O2, -O3)

#### Documentation
- `TWO_PASS_ARCHITECTURE.md` - Complete architecture overview
- `2_pass_plan.md` - Detailed implementation plan
- `2_pass_decision.md` - Final design decisions
- `2_pass_considerations.md` - Edge cases and details
- `2_pass_progress.md` - Current progress status
- `circular_dependency_analysis.md` - Architecture analysis

#### Tests
- `test/ir_test.c` - IR buffer unit tests

### Changed
- Updated `README.md` with two-pass architecture documentation
- Integrated IR buffer into compilation flow (`compile_unit.h`, `compile.h`)

### Technical Details

#### IR Structure (8 bytes packed)
```c
typedef struct __attribute__((packed)) {
    uint8_t  op;        // 0-255 opcodes
    uint8_t  temp_dest; // 0-255 temps (0xFF if none)
    uint8_t  temp_src1; // 0-255 temps (0xFF if none)
    uint8_t  temp_src2; // 0-255 temps (0xFF if none)
    int16_t  imm;       // -32768 to 32767
    uint16_t label_idx; // 0-65535 labels
} Instr;
```

#### Register Classes
- **GPR**: RAX, RCX, RDX, RBX, RSI, RDI, R8-R15 (14 registers)
- **XMM**: XMM0-XMM7 (8 registers for SSE2 float)

### Performance Targets

| Metric | -O0 | -O1 | -O2 | -O3 |
|--------|-----|-----|-----|-----|
| Compile Time | 1x | 1.2x | 3x | 10x |
| Code Speed | 1x | 2-5x | 5-10x | 10-20x |

### Next Steps (Phase 2)
- Define 50 opcodes (MOV, ADD, SUB, etc.)
- Migrate NODE_BINOP to IR
- Implement Pass 2 code emission
- Test with arithmetic expressions

---

## [Previous Versions]

### Sprint 2: AST Lowering and JIT Execution
- Single-pass code generation
- Global variable storage emission
- All 71 tests passing
- Zero compiler warnings

### Sprint 1: Core Compiler Infrastructure
- Parser and lexer
- Type system
- Semantic analysis
- Basic expression evaluation
