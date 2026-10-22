#!/usr/bin/env python3
"""ZeroCC style checker (ruff-style, stdlib-only, fast).

Enforces the conventions in docs/coding_style.md for C files under the
include roots (default: src/ include/).  Two severities:

  error  - convention violation; counted, drives the exit code
  warn   - prescriptive; reported for awareness, does not drive the exit
           code (the existing tree has accepted legacy instances)

Rules
-----
  INC001 [error]  quoted include uses ../ or ./ path navigation
  INC002 [error]  quoted include resolved only from the file's own
                  directory (sibling/parent-relative) - use the
                  include-root relative form ("zero_cc/...")
  INC003 [error]  quoted project include resolves to no file under an
                  include root (typo / deleted file)
  INC010 [warn]   a system include appears after project includes
                  (prescribed order: sibling, system, project; the
                  existing tree is mixed and not churned)
  ANG001 [error]  project header included with angle brackets
  WS101  [error]  tab in leading whitespace (tree is uniformly spaced)
  WS102  [error*] trailing whitespace            (* --fix repairs)
  WS103  [error*] file does not end with a newline (* --fix repairs)
  HDR001 [error]  .h has no include guard (#pragma once or
                  #ifndef/#define pair before the first include)
  NAM001 [error]  top-level non-static function lacks a module prefix
                  (the <module>_<action> underscore pattern); `main`
                  exempt.  The tree uses per-module prefixes — zero_cc_*
                  for core library, opt_*, diag_*, type_*, stmt_*, ir_*,
                  lower_*, emit_*, elf_writer_*, vectorization_*, sem_*,
                  arena_*, <isa>_* for backends, pthread_* for the
                  <threads.h> emulation (mirroring the system API).
  STR010 [warn]   unbounded string call (strcpy/strcat/sprintf/gets);
                  house rule prefers the _n/snprintf forms when the
                  bound is not locally obvious

Dormant-include allowlist (headers not yet created; see
docs/coding_style.md §2.1): zero_elf.h, zero_posix_baremetal.h

Usage:
  python3 tools/style_check.py            # check src/ include/
  python3 tools/style_check.py --fix      # repair WS102/WS103 in place
  python3 tools/style_check.py --dirs src include test
  python3 tools/style_check.py --quiet    # failures only

Exit code: 1 if any ERROR is found (after --fix where applicable), 0
otherwise.  Warnings never affect the exit code.
"""

import argparse
import os
import re
import sys

# --------------------------------------------------------------------------
# Rule data
# --------------------------------------------------------------------------

INCLUDE_ROOTS = ("src", "include")

# Quoted includes that intentionally reference headers not yet created
# (dormant freestanding placeholders).  See coding_style.md §2.1.
DORMANT_INCLUDES = {"zero_elf.h", "zero_posix_baremetal.h"}

QUOTED_RE = re.compile(r'^(\s*)#\s*include\s*"([^"]+)"(.*)$')
ANGLED_RE = re.compile(r'^\s*#\s*include\s+<([^>]+)>')
PROJECT_ANGLED = re.compile(r'^(zero_cc|zero_elf|threads)\b')

FUNC_DEF_RE = re.compile(
    r'^(?:inline\s+)?(?:unsigned\s+|signed\s+|const\s+|struct\s+|enum\s+)*'
    r'([A-Za-z_]\w*)(?:\s*\*)?\s+([A-Za-z_]\w*)\s*\(')
FUNC_RET_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "else", "do", "goto",
}

UNSAFE_STR_RE = re.compile(r'\b(strcpy|strcat|sprintf|gets)\s*\(')


# --------------------------------------------------------------------------
# Per-file checks
# --------------------------------------------------------------------------

def is_project_name(name):
    """Heuristic: does this include name refer to a project header?"""
    return bool(PROJECT_ANGLED.match(name))


def check_file(path, fix=False):
    """Return (errors, warnings, n_fixed)."""
    errors, warnings = [], []
    data = open(path, "rb").read()
    txt = data.decode("utf-8", "replace")
    fixed_here = 0

    # --- whole-file whitespace rules (WS103 / WS102) --------------------
    ended_nl = txt.endswith("\n")
    body = txt.rstrip("\n")
    if not ended_nl:
        errors.append((0, "WS103", "file does not end with a newline"))
    lines = body.split("\n")
    trail = [(i + 1, l) for i, l in enumerate(lines) if l != l.rstrip()]
    for ln, l in trail:
        errors.append((ln, "WS102", "trailing whitespace"))
    if fix and (trail or not ended_nl):
        out = "\n".join(l.rstrip() for l in lines) + "\n"
        open(path, "w").write(out)
        fixed_here = len(trail) + (0 if ended_nl else 1)

    # --- line-based rules -------------------------------------------------
    saw_project = False
    file_stem = os.path.splitext(os.path.basename(path))[0]
    for i, raw in enumerate(lines, 1):
        if raw.startswith("\t"):
            errors.append((i, "WS101", "tab in leading whitespace"))

        m = QUOTED_RE.match(raw)
        if m:
            inc = m.group(2)
            if inc.startswith("../") or inc.startswith("./") or "/../" in inc or "/./" in inc:
                errors.append((i, "INC001", f'include uses path navigation: "{inc}"'))
                continue
            if inc in DORMANT_INCLUDES:
                continue
            here = os.path.normpath(os.path.join(os.path.dirname(path), inc))
            at_root = any(
                os.path.isfile(os.path.join(r, inc)) for r in INCLUDE_ROOTS)
            if not os.path.exists(here) and not at_root:
                errors.append((i, "INC003", f'include resolves to no file: "{inc}"'))
                continue
            if os.path.exists(here) and not at_root:
                errors.append((i, "INC002",
                               f'non-canonical include "{inc}" — use the '
                               f"include-root relative form (\"zero_cc/...\" or \"<root>/...\")"))
            # INC010 is reported on the system-include side below: the
            # prescribed order (sibling, system, project) is violated when
            # a system include follows a project include.
            # (sibling detection uses the basename — project includes are
            # root-relative, e.g. "zero_cc/semantic/decl.h" for decl.c)
            if os.path.splitext(os.path.basename(inc))[0] != file_stem:
                saw_project = True
            continue

        a = ANGLED_RE.match(raw)
        if a and is_project_name(a.group(1)):
            errors.append((i, "ANG001",
                           f"project header with angle brackets: <{a.group(1)}>"))
            continue
        if a:
            # INC010: a system include after the first project include
            # (exempt: files that only use system headers).
            if saw_project:
                warnings.append((i, "INC010",
                                 "system include after project includes "
                                 "(prescribed order: sibling, system, project)"))
            continue

        # NAM001: top-level non-static function definition/declaration
        if raw[:1].isalpha() and not raw.startswith(
                ("static", "typedef", "struct", "enum", "union",
                 "#", "/*", "*", "//", "extern")):
            fm = FUNC_DEF_RE.match(raw)
            if fm:
                ret, name = fm.groups()
                if ret in FUNC_RET_KEYWORDS:
                    continue
                if name == "main":
                    continue
                if "_" not in name:
                    errors.append((i, "NAM001",
                                   f"top-level function '{name}' lacks a "
                                   "module prefix (<module>_<action>)"))

        sm = UNSAFE_STR_RE.search(raw)
        if sm and not raw.lstrip().startswith(("//", "*", "/*")):
            warnings.append((i, "STR010",
                             f"unbounded string call '{sm.group(1)}' "
                             f"(prefer the _n/snprintf form when the bound is not obvious)"))

    # --- HDR001: .h must have a guard -------------------------------------
    if path.endswith(".h"):
        has_pragma = False
        has_ifndef = False
        seen_include = False
        for line in lines:
            s = line.strip()
            if s.startswith("#pragma once"):
                has_pragma = True
                break
            if s.startswith("#ifndef"):
                name = s.split()[-1] if len(s.split()) > 1 else ""
                # find matching #define within a few lines
                idx = lines.index(line)
                for l2 in lines[idx + 1: idx + 5]:
                    l2 = l2.strip()
                    if l2.startswith("#define") and name and name in l2:
                        has_ifndef = True
                        break
                    if l2 and not l2.startswith(("#", "*", "/*")):
                        break
                break
            if s.startswith("#include"):
                seen_include = True
                break
            if s and not s.startswith(("#", "*", "/*", "//")):
                break  # code before any guard
        if not (has_pragma or has_ifndef):
            errors.append((1, "HDR001",
                           "header has no include guard (#pragma once or #ifndef/#define)"))

    return errors, warnings, fixed_here


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dirs", nargs="+", default=list(INCLUDE_ROOTS),
                    help="directories to scan (default: src include)")
    ap.add_argument("--fix", action="store_true",
                    help="repair WS102 (trailing whitespace) and "
                         "WS103 (missing final newline) in place")
    ap.add_argument("--quiet", action="store_true", help="print failures only")
    args = ap.parse_args()

    files = []
    for d in args.dirs:
        for dp, _, ns in os.walk(d):
            for n in sorted(ns):
                if n.endswith((".c", ".h")):
                    files.append(os.path.join(dp, n))

    n_err = n_warn = n_fixed = 0
    for p in files:
        errs, warns, fixed = check_file(p, fix=args.fix)
        n_fixed += fixed
        for ln, code, msg in sorted(errs):
            n_err += 1
            if not args.quiet or code not in ("WS102", "WS103"):
                print(f"{p}:{ln}: [{code}, error] {msg}")
        for ln, code, msg in sorted(warns):
            n_warn += 1
            if not args.quiet:
                print(f"{p}:{ln}: [{code}, warn] {msg}")

    print(f"\nstyle_check: {len(files)} files, {n_err} error(s), "
          f"{n_warn} warning(s)" + (f", {n_fixed} line(s) fixed" if args.fix else ""))
    return 1 if n_err else 0


if __name__ == "__main__":
    sys.exit(main())
