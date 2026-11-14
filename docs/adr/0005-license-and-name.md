# ADR-0005 — License MIT, product name FlowGet

- **Status:** Accepted
- **Date:** 2026-11-14
- **Deciders:** project owner

## Context

The project is a first open-source release intended to attract users and contributors, and possibly
to be embedded in other tools later.

## Decision

- **Code:** MIT. No patent grant, but the lowest possible barrier to use and contribution.
- **Name:** FlowGet is kept as the product name and repository name.
- **Trademark:** the name and logo are not licensed under MIT; a short note is added to the README
  before the first release. A name/trademark search (GitHub, npm, PyPI, AMO, EU/USPTO) is a release
  checklist item, not a blocker for development.

## Consequences

- Contributors keep their own copyright; no CLA is required for v1.
- If a corporate contributor later needs a patent grant, Apache-2.0 can be adopted for future
  versions only with explicit consent — not retroactively.
- If the name collides with an existing product, renaming affects the package IDs
  (`com.flowget.app`) and installer, so the search should happen before M7 packaging.

## Rejected alternatives

| Option | Why rejected |
|---|---|
| Apache-2.0 | NOTICE file overhead and no current patent concern; can be revisited |
| GPL-3.0 | Blocks embedding, contradicts the goal of broad adoption |
| BSL / source-available | Not open source in the OSI sense |
