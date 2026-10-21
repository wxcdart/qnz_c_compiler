#!/usr/bin/env python3
"""vtable_check.py - Multi-ISA backend vtable slot conformance & caller auditor (T3).

Audits ZeroCCISABackend struct definitions against:
1. The master struct definition in `src/zero_cc/isa/isa_backend.h`.
2. Actual call sites across `src/` (`BE(c)-><slot>` and `backend-><slot>`).
3. Peer backends (x86_64_backend, aarch64_backend, riscv64_backend vs riscv32_backend).

Pinpoints NULL or missing slots in backends that are actively invoked by the compiler
pipeline (direct causes of compiler segfaults).

Usage:
    python3 tools/vtable_check.py                     # audit all backends
    python3 tools/vtable_check.py --target riscv32   # audit riscv32_backend specifically
    python3 tools/vtable_check.py --active-only       # only show actively-called slots
    python3 tools/vtable_check.py --diff riscv64 riscv32

Exit: 0 = no active NULL slots, 1 = active slots are NULL in tested backend(s).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ISA_DIR = REPO / "src" / "zero_cc" / "isa"
SRC_DIR = REPO / "src"


def parse_struct_fields(header_path: Path) -> list[str]:
    """Extract field names from ZeroCCISABackend definition in isa_backend.h."""
    text = header_path.read_text(encoding="utf-8")
    m = re.search(r'typedef\s+struct\s+ZeroCCISABackend\s*\{([^}]+)\}\s*ZeroCCISABackend;', text, re.DOTALL)
    if not m:
        m = re.search(r'struct\s+ZeroCCISABackend\s*\{([^}]+)\};', text, re.DOTALL)
    if not m:
        raise ValueError("Could not find struct ZeroCCISABackend in header")

    body = m.group(1)
    fields = []
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("//") or line.startswith("/*") or line.startswith("*"):
            continue
        line = re.sub(r'/\*.*?\*/', '', line).strip()
        # Match function pointer: void (*name)(...) or field: int name;
        fm = re.search(r'\(\s*\*\s*([a-zA-Z0-9_]+)\s*\)', line)
        if fm:
            fields.append(fm.group(1))
        else:
            dm = re.search(r'([a-zA-Z0-9_]+)\s*;', line)
            if dm:
                fields.append(dm.group(1))
    return fields


def parse_backend_init(file_path: Path, backend_name: str) -> dict[str, str]:
    """Extract initialized slots from a backend struct definition."""
    text = file_path.read_text(encoding="utf-8")
    pattern = r'ZeroCCISABackend\s+' + re.escape(backend_name) + r'\s*=\s*\{([^;]+)\};'
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        return {}

    body = m.group(1)
    slots = {}
    for line in body.splitlines():
        line = line.strip()
        line = re.sub(r'/\*.*?\*/', '', line).strip()
        sm = re.match(r'\.([a-zA-Z0-9_]+)\s*=\s*([^,]+),?', line)
        if sm:
            slots[sm.group(1)] = sm.group(2).strip()
    return slots


def parse_lazy_asm_init(file_path: Path, func_name: str) -> dict[str, str]:
    """Extract slots initialized in init_asm_emission functions."""
    if not file_path.exists():
        return {}
    text = file_path.read_text(encoding="utf-8")
    pattern = r'void\s+' + re.escape(func_name) + r'\s*\([^)]*\)\s*\{([^}]+)\}'
    m = re.search(pattern, text, re.DOTALL)
    if not m:
        return {}

    slots = {}
    for line in m.group(1).splitlines():
        line = line.strip()
        sm = re.search(r'->([a-zA-Z0-9_]+)\s*=\s*([^;]+);', line)
        if sm:
            slots[sm.group(1)] = sm.group(2).strip()
    return slots


def find_active_call_sites(src_root: Path) -> dict[str, int]:
    """Scan all source files for BE(c)->slot or backend->slot invocations."""
    counts: dict[str, int] = {}
    for p in src_root.rglob("*.[ch]"):
        try:
            content = p.read_text(encoding="utf-8")
        except Exception:
            continue
        for m in re.finditer(r'(?:BE\s*\(\s*c\s*\)|backend)\s*->\s*([a-zA-Z0-9_]+)', content):
            slot = m.group(1)
            counts[slot] = counts.get(slot, 0) + 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target", choices=["x86_64", "aarch64", "riscv64", "riscv32"],
                    help="audit a specific backend target")
    ap.add_argument("--active-only", action="store_true",
                    help="only report slots that are actively called via BE(c)->")
    ap.add_argument("--diff", nargs=2, metavar=("BACKEND1", "BACKEND2"),
                    help="compare two backends directly (e.g. --diff riscv64 riscv32)")
    args = ap.parse_args()

    header = ISA_DIR / "isa_backend.h"
    all_fields = parse_struct_fields(header)
    active_calls = find_active_call_sites(SRC_DIR)

    # Load all backends
    backends = {
        "x86_64": parse_backend_init(ISA_DIR / "x86_64.c", "x86_64_backend"),
        "aarch64": parse_backend_init(ISA_DIR / "aarch64.c", "aarch64_backend"),
        "riscv64": parse_backend_init(ISA_DIR / "riscv.c", "riscv64_backend"),
        "riscv32": parse_backend_init(ISA_DIR / "riscv.c", "riscv32_backend"),
    }

    # Merge lazy ASM emission slots
    backends["x86_64"].update(parse_lazy_asm_init(ISA_DIR / "x86_64_asm.c", "x86_init_asm_emission"))
    backends["aarch64"].update(parse_lazy_asm_init(ISA_DIR / "aarch64_asm.c", "aarch64_init_asm_emission"))
    backends["riscv64"].update(parse_lazy_asm_init(ISA_DIR / "riscv_asm.c", "riscv64_init_asm_emission"))

    print(f"ZeroCC VTable Slot Auditor: {len(all_fields)} total struct slots, "
          f"{len(active_calls)} actively called via BE(c)->/backend->\n")

    if args.diff:
        b1, b2 = args.diff
        s1, s2 = backends.get(b1, {}), backends.get(b2, {})
        only1 = set(s1.keys()) - set(s2.keys())
        only2 = set(s2.keys()) - set(s1.keys())
        print(f"=== Direct Diff: {b1} ({len(s1)} slots) vs {b2} ({len(s2)} slots) ===")
        print(f"\nSlots in {b1} but MISSING in {b2} ({len(only1)}):")
        for slot in sorted(only1):
            calls = active_calls.get(slot, 0)
            tag = f"[ACTIVE: {calls} calls]" if calls > 0 else "[dormant]"
            print(f"  - {slot:<28} {tag}")
        print(f"\nSlots in {b2} but MISSING in {b1} ({len(only2)}):")
        for slot in sorted(only2):
            calls = active_calls.get(slot, 0)
            tag = f"[ACTIVE: {calls} calls]" if calls > 0 else "[dormant]"
            print(f"  - {slot:<28} {tag}")
        return 0

    targets = [args.target] if args.target else ["x86_64", "aarch64", "riscv64", "riscv32"]
    has_active_null = False

    for target in targets:
        slots = backends[target]
        missing_active = []
        missing_inactive = []

        for field in all_fields:
            val = slots.get(field)
            is_null = (val is None or val == "NULL")
            calls = active_calls.get(field, 0)

            if is_null:
                if calls > 0:
                    missing_active.append((field, calls))
                else:
                    missing_inactive.append(field)

        status = "FAIL (CRITICAL SLOTS MISSING)" if missing_active else "PASS"
        print(f"=== Target: {target} ({len(slots)} initialized) -> {status} ===")

        if missing_active:
            has_active_null = True
            print(f"  CRITICAL: {len(missing_active)} actively called slot(s) are NULL/uninitialized:")
            for field, calls in sorted(missing_active, key=lambda x: -x[1]):
                print(f"    ! {field:<28} (called {calls} time(s) across codebase)")

        if not args.active_only and missing_inactive:
            print(f"  Unused/dormant uninitialized slots: {len(missing_inactive)}")

        print()

    print("OVERALL RESULT:", "FAIL" if has_active_null else "PASS")
    return 1 if has_active_null else 0


if __name__ == "__main__":
    sys.exit(main())
