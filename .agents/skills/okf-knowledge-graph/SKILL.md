---
name: okf-knowledge-graph
description: >-
  Use this skill to navigate, query, update, and validate the Open Knowledge Format (OKF v0.2)
  knowledge bundle in docs/okf/ whenever researching project architecture or recording new
  architectural decisions, ABI/API rules, or invariants.
---

# Open Knowledge Format (OKF v0.2) Skill

This skill provides the workflow for traversing and maintaining the structured knowledge graph in [`docs/okf/index.md`](../../../docs/okf/index.md).

## 1. Navigating & Searching the Knowledge Graph (Query-First)

1. **Always start at [`docs/okf/index.md`](../../../docs/okf/index.md).**
2. Follow relative markdown links from `index.md` to the specific domain concept page you need.
3. **Ranked Search & Table of Contents**: Before falling back to raw `grep` across legacy docs, use `tools/okf.py`:
   ```bash
   # Ranked keyword search across title (5x), tags/resource (4x), description (3x), headings (2x), body (1x)
   python3 tools/okf.py search <keywords>

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
timestamp: 2026-10-16T03:12:40Z
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
  python3 tools/okf.py update <page.md> --log "Describe the architectural update"
  ```

## 3. Validating the OKF Bundle

After creating or editing any file in `docs/okf/`, always run:

```bash
python3 tools/okf.py lint
python3 tools/okf.py index
```

Ensure both report `[PASS]` / `[OK]` with `0` broken links, valid YAML frontmatter, and 100% reachability from `index.md`.
