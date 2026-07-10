# PJ4 top-level `docs/`

This folder is for **cross-cutting documentation only** — material that doesn't fit inside any single module's `docs/` folder.

> **Qt toolchain:** PJ4 builds against **Qt 6.11.1**. Install it with the repo-root
> [`../install_qt6.sh`](../install_qt6.sh) — the single source of truth for the
> Linux Qt version (`build.sh`, `run.sh`, and Linux CI all consume it). See
> [`QT_NOTES.md`](./QT_NOTES.md) for what changed since 6.8.

Examples of what belongs here:

- Porting strategy / migration notes that span multiple modules.
- Glossary or terminology references used across the repo.
- Architecture decision records (ADRs) for choices that affect multiple modules.
- **Research notes** that informed multiple modules' designs — under `research/`.
- Work-in-progress design specs for modules that don't yet have their own `docs/`.

## What does NOT belong here

- **Module-local intent docs** (REQUIREMENTS, ARCHITECTURE, USER_MANUAL, …) — those live in `<module>/docs/`.
  Examples in this repo: `pj_scene2D/docs/`, `pj_marketplace/docs/`.
- **Plan documents that are repo-wide** — `PJ4_PLAN.md` lives at the repo root.
- **Agent-management state** (`.remember/`, `.claude/`, `.agents/`) — those are not human-readable docs.

## Current contents

| Path | Status | Notes |
|---|---|---|
| `QT_NOTES.md` | Reference | **Qt 6.11.1 baseline.** Features new since 6.8 (likely past most models' training cutoff), deprecations, and build/platform floors. Read before using an unfamiliar Qt API or reaching for a 6.8-era workaround. |
| `undo_redo_integrity_plan.md` | Plan (active) | Consolidated undo/redo state-integrity delivery plan: dataset identity, transactional undo, timeline-state snapshots, processor/scene persistence — five incremental PRs distilled from two prior solution branches. |
| `research/dataset_format_comparison.md` | Reference | Cross-cutting comparison of MCAP, RLDS, LeRobot, Zarr. Informs pj_scene2D and any future dataset-format work. |
| `research/rerun_notes.md` | Reference | Analysis of Rerun's 2D architecture; comparison input for pj_scene2D and (potentially) pj_scene3D. |

## Where to find module docs

See the "Module documentation index" in the top-level [`CLAUDE.md`](../CLAUDE.md). Each PJ4 module owns a `CLAUDE.md` at its root and, when warranted, a `docs/` folder with `REQUIREMENTS.md` / `ARCHITECTURE.md`.
