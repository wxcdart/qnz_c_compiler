---
name: okf-knowledge-graph
description: >-
  Use this skill to navigate, query, update, and validate the ZeroCC Open Knowledge Format (OKF v0.2)
  knowledge bundle in docs/okf/ whenever researching compiler internals or recording new architectural
  decisions, ABI rules, or optimization invariants.
---

# ZeroCC Open Knowledge Format (OKF v0.2) Skill

This skill provides the workflow for traversing and maintaining ZeroCC's structured knowledge graph in [`docs/okf/`](../../../docs/okf/index.md).

## 1. Navigating & Searching the Knowledge Graph (Query-First)

1. **Always start at [`docs/okf/index.md`](../../../docs/okf/index.md).**
2. Follow relative markdown links to the specific domain concept you need:
   - [architecture-pipeline.md](../../../docs/okf/architecture-pipeline.md)
   - [frontend-and-types.md](../../../docs/okf/frontend-and-types.md)
   - [ir-and-regalloc.md](../../../docs/okf/ir-and-regalloc.md)
   - [optimization-passes.md](../../../docs/okf/optimization-passes.md)
   - [abi-sysv-x86_64.md](../../../docs/okf/abi-sysv-x86_64.md)
   - [isa-backends.md](../../../docs/okf/isa-backends.md)
   - [build-and-verification.md](../../../docs/okf/build-and-verification.md)
3. **Ranked Search & Table of Contents**: Before falling back to raw `grep` across the 80+ legacy files in `docs/`, use the unified [`tools/okf.py`](../../../tools/okf.py) CLI:
   ```bash
   # Ranked keyword search across title (5x), tags/resource (4x), description (3x), headings (2x), body (1x)
   python3 tools/okf.py search sret funcptr

   # Print full Table of Contents with summaries and tags
   python3 tools/okf.py search --toc

   # Check bundle status and page timestamps
   python3 tools/okf.py status
   ```

## 2. Adding or Updating an OKF Concept Node

Every `.md` file in `docs/okf/` must begin with a valid OKF v0.2 YAML frontmatter block containing all required keys (`type`, `title`, `description`, `resource`, `tags`, `timestamp`, `status`):

```markdown
---
type: Concept
title: Concise Concept Title
description: One-sentence summary of the architectural rule or subsystem.
resource: category/slug
tags: [tag1, tag2, tag3]
timestamp: 2026-10-15T23:00:00Z
generated: antigravity-agent
verified: engineering-lead
status: active
---
```

### Graph Linking & Updating Rules
- Link every new concept file from [`docs/okf/index.md`](../../../docs/okf/index.md) using a relative path (`[Title](./my-concept.md)`).
- Include a `## Related Concepts` section at the bottom of each concept file with relative links back to `[Root Index](./index.md)` and related peer nodes.
- After editing a concept page, bump its timestamp and append a changelog entry to [`docs/okf/log.md`](../../../docs/okf/log.md):
  ```bash
  python3 tools/okf.py update abi-sysv-x86_64.md --log "Document new ABI rule"
  ```

## 3. Validating the OKF Bundle

After creating or editing any file in `docs/okf/`, always run the validator:

```bash
python3 tools/okf.py lint
python3 tools/okf.py index
```

Ensure both report `[PASS]` / `[OK]` with `0` broken links, valid YAML frontmatter, and 100% reachability from `index.md`.
