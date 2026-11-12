"""ZeroCC verification tooling package.

Tools are run as standalone scripts (``python3 tools/<subdir>/<script>.py``),
not imported as a package.  Each tool locates the repository root via
:func:`find_repo_root` instead of hardcoded ``os.path.dirname`` chains, so
moving the tools folder (or nesting it deeper) never breaks path resolution.
"""

from pathlib import Path

# Files that only exist at the repository root; used to locate it robustly.
_MARKERS = ("AGENTS.md", "Makefile", "docs/okf/index.md")


def find_repo_root(start: Path | None = None) -> Path:
    """Walk up from *start* (default: the directory of this module) until a
    marker file is found, then return that directory.

    Falls back to *start* if no marker is found within 10 levels (defensive;
    should never happen in a normal ZeroCC checkout).
    """
    if start is None:
        start = Path(__file__).resolve().parent  # = tools/
    cur = start.resolve()
    for _ in range(10):
        if any((cur / m).exists() for m in _MARKERS):
            return cur
        if cur.parent == cur:
            break
        cur = cur.parent
    return cur
