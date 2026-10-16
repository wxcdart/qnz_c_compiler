---
type: Guide
title: Build, Test & Verification Guide for Agents
description: Practical commands and rules for building ZeroCC, running the 136-test suite, testing JIT vs AOT compilation, and running ABI/OKF verifiers.
resource: workflows/build-and-verification
tags: [build, testing, verification, makefile, sret, okf]
timestamp: 2026-10-15T22:55:00Z
generated: antigravity-agent
verified: engineering-lead
status: active
---

# Build, Test & Verification Guide for Agents

Follow these workflows whenever modifying ZeroCC.

## 1. Building ZeroCC

> **Note**: The Makefile target is `make all` or `make build/bin/zerocc` (there is no `make build` target). Incremental builds track header dependencies automatically via `-MMD -MP`.

```bash
# Build compiler binary and all test binaries
make all

# Build only the ZeroCC compiler binary
make build/bin/zerocc

# Full clean rebuild (use when changing Makefile or switching branches)
make rebuild
```

## 2. Running the Test Suite

```bash
# Run all 136 tests (unit, semantic, abi, jit)
make test

# Run specific category
make test-abi
make test-jit
make test-semantic
make test-unit
```

## 3. Verifying Individual C Files with ZeroCC Directly

Note that `build/test/abi/*` binaries built by `make all` are compiled by the host `$(CC)` (`gcc`), whereas `test/jit/*` and `tools/sret_verifier.py` invoke `./build/bin/zerocc`. **Always test new ABI features directly with `./build/bin/zerocc` in both AOT and JIT modes**:

```bash
# 1. AOT Executable mode (-o)
./build/bin/zerocc test/abi/struct_return_funcptr.c -o /tmp/test_bin && /tmp/test_bin

# 2. JIT Execution mode (--run)
./build/bin/zerocc --run test/abi/struct_return_funcptr.c

# 3. Inspect generated assembly (-S) or IR (--dump-ir)
./build/bin/zerocc -S test/abi/struct_return_funcptr.c -o /tmp/test.s
```

## 4. Automated ABI & OKF Verification Tools

```bash
# Verify System V AMD64 sret (>16B struct return) callee & caller assembly patterns (6/6 checks)
python3 tools/sret_verifier.py --verify-all

# Verify struct ABI classification and register usage
python3 tools/struct_abi_verifier.py --test test/abi/struct_return_20byte.c

# Validate the docs/okf/ Open Knowledge Format bundle (YAML frontmatter & graph links)
python3 tools/okf.py lint
python3 tools/okf.py index
```

## Related Concepts
- Return to [Root Index](./index.md)
- See [Bundle Log](./log.md)
- See [System V AMD64 ABI & Struct Calling Conventions](./abi-sysv-x86_64.md)
- See [Compilation Pipeline & Two-Pass Architecture](./architecture-pipeline.md)
