#!/usr/bin/env python3
"""
Open Knowledge Format (OKF v0.2) Bundle Validator for ZeroCC.
Checks every .md file in docs/okf/ for:
1. Valid YAML frontmatter block with required OKF v0.2 fields (type, title, description, resource, tags, timestamp, status).
2. Valid relative markdown links ([label](./target.md)) that resolve to existing files.
3. Graph reachability from index.md.
"""

import os
import re
import sys

REQUIRED_KEYS = {"type", "title", "description", "resource", "tags", "timestamp", "status"}
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def parse_frontmatter(text):
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None, "Missing opening '---' YAML frontmatter delimiter"
    end_idx = None
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            end_idx = i
            break
    if end_idx is None:
        return None, "Missing closing '---' YAML frontmatter delimiter"

    meta = {}
    for line in lines[1:end_idx]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if ":" not in stripped:
            return None, f"Invalid frontmatter line: {line}"
        k, v = stripped.split(":", 1)
        meta[k.strip()] = v.strip()
    return meta, None


def validate_bundle(bundle_dir):
    if not os.path.isdir(bundle_dir):
        print(f"[ERROR] OKF directory not found: {bundle_dir}")
        return 1

    md_files = sorted(f for f in os.listdir(bundle_dir) if f.endswith(".md"))
    if "index.md" not in md_files:
        print("[ERROR] Missing required root navigator file: index.md")
        return 1
    if "log.md" not in md_files:
        print("[ERROR] Missing required sequential changelog file: log.md")
        return 1

    errors = []
    edges = {f: set() for f in md_files}

    for fname in md_files:
        fpath = os.path.join(bundle_dir, fname)
        with open(fpath, "r", encoding="utf-8") as f:
            content = f.read()

        meta, err = parse_frontmatter(content)
        if err:
            errors.append(f"{fname}: {err}")
        else:
            missing = REQUIRED_KEYS - set(meta.keys())
            if missing:
                errors.append(f"{fname}: Missing required frontmatter keys: {sorted(missing)}")

        for _label, target in LINK_RE.findall(content):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target_path = target.split("#", 1)[0]
            resolved = os.path.normpath(os.path.join(bundle_dir, target_path))
            if not os.path.exists(resolved):
                errors.append(f"{fname}: Broken relative link -> {target}")
            elif os.path.dirname(resolved) == os.path.abspath(bundle_dir):
                edges[fname].add(os.path.basename(resolved))

    # Check reachability from index.md
    visited = set()
    stack = ["index.md"]
    while stack:
        curr = stack.pop()
        if curr in visited:
            continue
        visited.add(curr)
        for nxt in edges.get(curr, ()):
            if nxt not in visited:
                stack.append(nxt)

    unreachable = set(md_files) - visited
    if unreachable:
        errors.append(f"Unreachable OKF files from index.md: {sorted(unreachable)}")

    print("======================================================================")
    print("OKF BUNDLE VALIDATION SUMMARY")
    print("======================================================================")
    print(f"Bundle path : {bundle_dir}")
    print(f"Files found : {len(md_files)} ({', '.join(md_files)})")
    print(f"Graph edges : {sum(len(v) for v in edges.values())} internal links")
    print(f"Reachable   : {len(visited)}/{len(md_files)} from index.md")

    if errors:
        print("\n[FAIL] Validation errors:")
        for e in errors:
            print(f"  - {e}")
        return 1

    print("\n[PASS] All OKF v0.2 frontmatter schemas, relative links, and graph reachability verified!")
    print("======================================================================")
    return 0


if __name__ == "__main__":
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_dir = os.path.join(repo_root, "docs", "okf")
    target_dir = sys.argv[1] if len(sys.argv) > 1 else default_dir
    sys.exit(validate_bundle(target_dir))
