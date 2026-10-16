---
name: okf-knowledge-graph
description: >-
  Use this skill to navigate, query, update, and validate the ZeroCC Open Knowledge Format (OKF v0.2)
  knowledge bundle in docs/okf/ whenever researching compiler internals or recording new architectural
  decisions, ABI rules, or optimization invariants.
---

# ZeroCC Open Knowledge Format (OKF v0.2) Skill

This skill provides the workflow for traversing and maintaining ZeroCC's structured knowledge graph in [`docs/okf/`](../../../docs/okf/index.md).

## 1. Navigating the Knowledge Graph

1. Always start at [`docs/okf/index.md`](../../../docs/okf/index.md).
2. Follow relative markdown links to the specific domain concept you need:
   - [architecture-pipeline.md](../../../docs/okf/architecture-pipeline.md)
   - [frontend-and-types.md](../../../docs/okf/frontend-and-types.md)
   - [ir-and-regalloc.md](../../../docs/okf/ir-and-regalloc.md)
   - [optimization-passes.md](../../../docs/okf/optimization-passes.md)
   - [abi-sysv-x86_64.md](../../../docs/okf/abi-sysv-x86_64.md)
   - [isa-backends.md](../../../docs/okf/isa-backends.md)
   - [build-and-verification.md](../../../docs/okf/build-and-verification.md)
3. Check [`docs/okf/log.md`](../../../docs/okf/log.md) for the most recent architectural updates.

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

### Graph Linking Rules
- Link every new concept file from [`docs/okf/index.md`](../../../docs/okf/index.md) using a relative path (`[Title](./my-concept.md)`).
- Include a `## Related Concepts` section at the bottom of each concept file with relative links back to `[Root Index](./index.md)` and related peer nodes.
- Record all additions or updates in [`docs/okf/log.md`](../../../docs/okf/log.md).

## 3. Validating the OKF Bundle

After creating or editing any file in `docs/okf/`, always run the validator script from the repository root:

```bash
python3 tools/okf_validator.py
```

Ensure it reports `[PASS]` with `0` broken links, valid YAML frontmatter, and 100% reachability from `index.md`.
