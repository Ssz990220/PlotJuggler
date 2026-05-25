# PlotJuggler Marketplace — TODO

This file tracks remaining work that is intentionally deferred or still
open. Items here are referenced from `REQUIREMENTS.md` (e.g. F-11
"local registry cache with TTL" and F-13 "automatic rollback").

> Repo-wide documentation freshness is governed by the rule in the
> root [`CLAUDE.md`](../../CLAUDE.md) "Documentation → Freshness
> discipline" section; it does not need re-listing here.

## Product follow-ups

- **Automatic rollback / restore from backup after a failed plugin load.**
  Backups are written today (F-12 is implemented), but the restore step
  is manual — `UC-04` in REQUIREMENTS notes the user is notified and the
  backup is left in place. (REQUIREMENTS.md F-13.)
- **Local registry cache with TTL, *if* we decide to reintroduce caching.**
  `RegistryManager` always fetches fresh today. Caching adds staleness
  surface for limited benefit at the current catalogue size; reopen only
  with a concrete need. (REQUIREMENTS.md F-11.)
- **macOS packaging and runtime validation.** Module compiles on macOS
  per the build matrix in spec §8; end-to-end runtime validation on
  macOS is not part of regular CI yet.

## Maintenance follow-ups

- **Add Windows/macOS coverage** where CI or local runners make that
  practical (currently Linux-first; Windows has continue-on-error per
  the project-wide CI note).
- **Revisit registry-side metadata decisions** only if the contract
  changes again — the schema in REQUIREMENTS.md §11.1 is the source of
  truth.
