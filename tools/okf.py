#!/usr/bin/env python3
"""
Unified Open Knowledge Format (OKF v0.2) CLI for ZeroCC (`docs/okf/`).
Zero external dependencies (Python 3 stdlib only).

Subcommands:
  lint   [bundle]               Validate YAML frontmatter, relative links, and reachability
  search <query...> [--toc]     Ranked search across OKF concept pages (title/tags/desc/headings/body)
  status [bundle]               Display bundle health, page counts, graph edges, and recent updates
  update <page> [--log MSG]     Bump frontmatter ISO-8601 timestamp and optionally append to log.md
  index  [bundle] [--check]     Check or synchronize concept listing in index.md
  now                           Print current UTC ISO-8601 timestamp
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
import re
import sys
from pathlib import Path

REQUIRED_KEYS = {"type", "title", "description", "resource", "tags", "timestamp", "status"}
LINK_RE = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")
HEADING_RE = re.compile(r"^#{1,6}\s+(.+)$", re.MULTILINE)
TOKEN_RE = re.compile(r"[a-zA-Z0-9_]+")


def default_bundle_dir() -> Path:
    # Check if run from tools/ or .agents/skills/okf-knowledge-graph/scripts/
    here = Path(__file__).resolve().parent
    for candidate in [here.parent / "docs" / "okf", here.parent.parent.parent.parent / "docs" / "okf", Path.cwd() / "docs" / "okf"]:
        if candidate.is_dir():
            return candidate
    return Path.cwd() / "docs" / "okf"


def now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_page(fpath: Path) -> tuple[dict[str, str] | None, str, str | None]:
    """Return (frontmatter_dict, body_text, error_message)."""
    text = fpath.read_text(encoding="utf-8")
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None, text, "Missing opening '---' YAML frontmatter delimiter"
    end_idx = None
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            end_idx = i
            break
    if end_idx is None:
        return None, text, "Missing closing '---' YAML frontmatter delimiter"

    meta: dict[str, str] = {}
    for line in lines[1:end_idx]:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if ":" not in stripped:
            return None, text, f"Invalid frontmatter line: {line}"
        k, v = stripped.split(":", 1)
        meta[k.strip()] = v.strip()

    body = "\n".join(lines[end_idx + 1 :])
    return meta, body, None


def cmd_lint(bundle_dir: Path) -> int:
    if not bundle_dir.is_dir():
        print(f"[ERROR] OKF directory not found: {bundle_dir}", file=sys.stderr)
        return 1

    md_files = sorted(f.name for f in bundle_dir.glob("*.md"))
    if "index.md" not in md_files:
        print("[ERROR] Missing required root navigator file: index.md", file=sys.stderr)
        return 1
    if "log.md" not in md_files:
        print("[ERROR] Missing required sequential changelog file: log.md", file=sys.stderr)
        return 1

    errors: list[str] = []
    edges: dict[str, set[str]] = {f: set() for f in md_files}

    for fname in md_files:
        fpath = bundle_dir / fname
        content = fpath.read_text(encoding="utf-8")
        meta, _body, err = parse_page(fpath)
        if err:
            errors.append(f"{fname}: {err}")
        elif meta is not None:
            missing = REQUIRED_KEYS - set(meta.keys())
            if missing:
                errors.append(f"{fname}: Missing required frontmatter keys: {sorted(missing)}")

        for _label, target in LINK_RE.findall(content):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target_path = target.split("#", 1)[0]
            resolved = (bundle_dir / target_path).resolve()
            if not resolved.exists():
                errors.append(f"{fname}: Broken relative link -> {target}")
            elif resolved.parent == bundle_dir.resolve():
                edges[fname].add(resolved.name)

    visited: set[str] = set()
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


def cmd_search(bundle_dir: Path, query: str, toc: bool = False, as_json: bool = False, max_results: int = 10) -> int:
    if not bundle_dir.is_dir():
        print(f"[ERROR] OKF directory not found: {bundle_dir}", file=sys.stderr)
        return 1

    md_files = sorted(bundle_dir.glob("*.md"))
    if toc:
        rows = []
        for fpath in md_files:
            meta, _body, _err = parse_page(fpath)
            meta = meta or {}
            rows.append({
                "file": fpath.name,
                "type": meta.get("type", "Unknown"),
                "title": meta.get("title", fpath.stem),
                "resource": meta.get("resource", ""),
                "tags": meta.get("tags", "[]"),
                "description": meta.get("description", ""),
            })
        if as_json:
            print(json.dumps(rows, indent=2))
        else:
            print(f"OKF Table of Contents ({bundle_dir}):\n")
            for r in rows:
                print(f"- [{r['file']}] ({r['type']}) {r['title']}")
                print(f"    Resource : {r['resource']} | Tags: {r['tags']}")
                print(f"    Summary  : {r['description']}")
        return 0

    terms = [t.lower() for t in TOKEN_RE.findall(query) if t]
    if not terms:
        print("[ERROR] Empty search query. Provide search terms or use --toc.", file=sys.stderr)
        return 1

    results = []
    for fpath in md_files:
        meta, body, _err = parse_page(fpath)
        meta = meta or {}
        if meta.get("status", "active").lower() == "archived":
            continue

        title = meta.get("title", "").lower()
        tags = meta.get("tags", "").lower()
        resource = meta.get("resource", "").lower()
        desc = meta.get("description", "").lower()
        headings = " ".join(HEADING_RE.findall(body)).lower()
        body_lower = body.lower()

        score = 0
        matched_terms = []
        for term in terms:
            term_score = 0
            if term in title or term in fpath.stem.lower():
                term_score += 5
            if term in tags or term in resource:
                term_score += 4
            if term in desc:
                term_score += 3
            if term in headings:
                term_score += 2
            body_hits = body_lower.count(term)
            if body_hits > 0:
                term_score += min(body_hits, 5)
            if term_score > 0:
                matched_terms.append(term)
                score += term_score

        if score > 0:
            # Find best matching snippet line
            snippet = meta.get("description", "")
            for line in body.splitlines():
                sline = line.strip()
                if sline and not sline.startswith("#") and any(t in sline.lower() for t in terms):
                    snippet = sline[:140]
                    break
            results.append({
                "file": f"docs/okf/{fpath.name}",
                "title": meta.get("title", fpath.name),
                "type": meta.get("type", ""),
                "score": score,
                "matched_terms": matched_terms,
                "description": meta.get("description", ""),
                "snippet": snippet,
            })

    results.sort(key=lambda r: (-r["score"], r["file"]))
    results = results[:max_results]

    if as_json:
        print(json.dumps(results, indent=2))
        return 0

    if not results:
        print(f"No OKF pages matched query: '{query}'")
        return 0

    print(f"OKF Search Results for '{query}' ({len(results)} match(es)):\n")
    for idx, r in enumerate(results, 1):
        print(f"{idx}. {r['file']} [score={r['score']}, type={r['type']}] — {r['title']}")
        print(f"   Description: {r['description']}")
        if r["snippet"] and r["snippet"] != r["description"]:
            print(f"   Snippet    : {r['snippet']}")
    return 0


def cmd_status(bundle_dir: Path) -> int:
    if not bundle_dir.is_dir():
        print(f"[ERROR] OKF directory not found: {bundle_dir}", file=sys.stderr)
        return 1

    md_files = sorted(bundle_dir.glob("*.md"))
    active_count = 0
    archived_count = 0
    total_links = 0
    pages_info = []

    for fpath in md_files:
        content = fpath.read_text(encoding="utf-8")
        meta, _body, _err = parse_page(fpath)
        meta = meta or {}
        status = meta.get("status", "active").lower()
        if status == "archived":
            archived_count += 1
        else:
            active_count += 1
        links = [t for _l, t in LINK_RE.findall(content) if not t.startswith(("http://", "https://", "mailto:", "#"))]
        total_links += len(links)
        pages_info.append((meta.get("timestamp", ""), fpath.name, meta.get("type", ""), meta.get("title", "")))

    pages_info.sort(reverse=True)
    print("======================================================================")
    print("OKF BUNDLE STATUS")
    print("======================================================================")
    print(f"Bundle directory : {bundle_dir}")
    print(f"Total pages      : {len(md_files)} (active: {active_count}, archived: {archived_count})")
    print(f"Internal links   : {total_links}")
    print("\nPages by timestamp:")
    for ts, fname, ptype, title in pages_info:
        print(f"  - {fname:<28} [{ptype:<13}] {ts}  ({title})")
    print("======================================================================")
    return 0


def cmd_update(bundle_dir: Path, page_arg: str, log_msg: str | None) -> int:
    page_path = Path(page_arg)
    if not page_path.exists():
        candidate = bundle_dir / page_arg
        if not candidate.name.endswith(".md"):
            candidate = bundle_dir / f"{page_arg}.md"
        page_path = candidate

    if not page_path.exists():
        print(f"[ERROR] OKF page not found: {page_arg}", file=sys.stderr)
        return 1

    text = page_path.read_text(encoding="utf-8")
    ts = now_iso()
    new_text, count = re.subn(
        r"^(timestamp:\s*).*$",
        f"\\g<1>{ts}",
        text,
        count=1,
        flags=re.MULTILINE,
    )
    if count == 0:
        print(f"[ERROR] Could not find 'timestamp:' key in {page_path.name}", file=sys.stderr)
        return 1

    page_path.write_text(new_text, encoding="utf-8")
    print(f"[OK] Updated timestamp in {page_path.name} -> {ts}")

    if log_msg:
        log_path = bundle_dir / "log.md"
        if log_path.exists():
            log_text = log_path.read_text(encoding="utf-8")
            log_text, _ = re.subn(
                r"^(timestamp:\s*).*$",
                f"\\g<1>{ts}",
                log_text,
                count=1,
                flags=re.MULTILINE,
            )
            date_str = ts[:10]
            entry = f"\n## {date_str} — Updated `{page_path.name}`\n- {log_msg}\n"
            log_path.write_text(log_text.rstrip() + "\n" + entry, encoding="utf-8")
            print(f"[OK] Appended update entry to {log_path.name}")

    return 0


def cmd_index(bundle_dir: Path) -> int:
    index_path = bundle_dir / "index.md"
    if not index_path.exists():
        print(f"[ERROR] index.md not found in {bundle_dir}", file=sys.stderr)
        return 1

    index_text = index_path.read_text(encoding="utf-8")
    linked = {
        Path(t.split("#", 1)[0]).name
        for _l, t in LINK_RE.findall(index_text)
        if not t.startswith(("http://", "https://", "mailto:", "#"))
    }
    all_pages = {p.name for p in bundle_dir.glob("*.md") if p.name != "index.md"}
    unindexed = sorted(all_pages - linked)

    if not unindexed:
        print(f"[OK] All {len(all_pages)} peer pages in {bundle_dir} are linked from index.md.")
        return 0

    print(f"[WARN] Unindexed pages not directly linked in index.md: {unindexed}")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="ZeroCC Open Knowledge Format (OKF v0.2) CLI")
    sub = parser.add_subparsers(dest="subcommand")

    p_lint = sub.add_parser("lint", help="Validate YAML frontmatter, relative links, and graph reachability")
    p_lint.add_argument("bundle", nargs="?", default=None, help="Path to OKF bundle directory")

    p_search = sub.add_parser("search", help="Ranked search across OKF concept pages")
    p_search.add_argument("query", nargs="*", help="Search keywords")
    p_search.add_argument("--bundle", default=None, help="Path to OKF bundle directory")
    p_search.add_argument("--toc", action="store_true", help="Print table of contents of all OKF pages")
    p_search.add_argument("--json", action="store_true", help="Output JSON")
    p_search.add_argument("--max-results", type=int, default=10, help="Max results to return")

    p_status = sub.add_parser("status", help="Display bundle health and page timestamps")
    p_status.add_argument("bundle", nargs="?", default=None, help="Path to OKF bundle directory")

    p_update = sub.add_parser("update", help="Bump page frontmatter timestamp and optionally log")
    p_update.add_argument("page", help="Page filename or path (e.g. abi-sysv-x86_64.md)")
    p_update.add_argument("--log", default=None, help="Optional changelog entry to append to log.md")
    p_update.add_argument("--bundle", default=None, help="Path to OKF bundle directory")

    p_index = sub.add_parser("index", help="Verify all OKF pages are indexed in index.md")
    p_index.add_argument("bundle", nargs="?", default=None, help="Path to OKF bundle directory")

    sub.add_parser("now", help="Print current UTC ISO-8601 timestamp")

    args = parser.parse_args()

    if args.subcommand in (None, "lint"):
        bdir = Path(getattr(args, "bundle", None) or default_bundle_dir())
        return cmd_lint(bdir)
    if args.subcommand == "search":
        bdir = Path(args.bundle) if args.bundle else default_bundle_dir()
        return cmd_search(bdir, " ".join(args.query), toc=args.toc, as_json=args.json, max_results=args.max_results)
    if args.subcommand == "status":
        bdir = Path(args.bundle) if args.bundle else default_bundle_dir()
        return cmd_status(bdir)
    if args.subcommand == "update":
        bdir = Path(args.bundle) if args.bundle else default_bundle_dir()
        return cmd_update(bdir, args.page, args.log)
    if args.subcommand == "index":
        bdir = Path(args.bundle) if args.bundle else default_bundle_dir()
        return cmd_index(bdir)
    if args.subcommand == "now":
        print(now_iso())
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())
