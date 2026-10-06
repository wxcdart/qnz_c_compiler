#!/usr/bin/env python3
"""Search the source tree for calls to the generic callee‑save helper.

The function ``emit_callee_save_stores`` should only be invoked from the
x86‑specific lowering path.  Accidentally calling it unconditionally will cause
the x86 prologue to appear in AArch64 code.

Running this script prints every occurrence of the call, together with the
file name and line number, so you can verify that the ISA guard has been added
everywhere.
"""

import sys
from pathlib import Path
import re

def main() -> int:
    root = Path('src')
    pattern = re.compile(r"\bemit_callee_save_stores\b")
    found = False
    for py_path in root.rglob('*.c'):
        try:
            with py_path.open('r', encoding='utf-8') as f:
                for i, line in enumerate(f, start=1):
                    if pattern.search(line):
                        print(f"{py_path}:{i}: {line.rstrip()}")
                        found = True
        except Exception as e:
            print(f"[warning] could not read {py_path}: {e}", file=sys.stderr)
    if not found:
        print("No calls to emit_callee_save_stores found.")
    return 0

if __name__ == '__main__':
    sys.exit(main())
