# pj_datastore — columnar time-series storage engine

Level-0 foundation library — a **top-level PJ4 module** (it was previously part of the `plotjuggler_sdk` submodule and moved into the app repo, because plugins reach it only through the `pj_base` C ABI, never by linking it). Owns the in-memory columnar store that every plugin writes to and every consumer reads from: datasets/topics/chunks/columns, adaptive per-column encoding, schema evolution, derived-series DAG (`DerivedEngine`), the opaque-blob `ObjectStore`, and the host-side C-ABI bridges that translate `pj_plugins` calls into engine operations. Pure C++20 (fmt, tsl::robin_map, nanoarrow); **no Qt, no `pj_plugins` dependency** — `pj_datastore → pj_base` only (`pj_base` comes from the `plotjuggler_sdk` SDK submodule). It does NOT decode media, choose renderers, own UI/time-display policy, or know about plugin discovery (that is `pj_plugins`). Timestamps are absolute int64 nanoseconds; do not subtract a display base here. Licensed **MPL-2.0** (see `LICENSE`); each source file carries its SPDX header.

## Layout
- `include/pj_datastore/` — public headers (engine/writer/reader/query/chunk, `object_store`, `derived_engine` + `builtin_transforms`, `plugin_data_host`, `colormap_registry`, `arrow_import`, low-level buffer/column_buffer/encoding/topic_storage/type_registry).
- `src/` — implementations, one `.cpp` per header.
- `tests/` — one GTest binary per layer (see `CMakeLists.txt` for the live set; several v3-ABI tests are commented out pending Phase 1b).
- `benchmarks/` — `read_benchmark`, `ingest_benchmark`.
- `examples/` — `parquet_import` (gated by `PJ_BUILD_PARQUET_IMPORT_EXAMPLE`).
- `docs/` — see table below.

## Gotchas
- **`readNumericAsDouble()` does not null-check** — returns 0.0 at nulls. Use `isNull()` first, or batch via `readColumnAsDoubles()` which writes NaN at nulls. See `docs/USER_GUIDE.md §5`.
- **Columns can appear mid-stream**: a new field after rows exist seals the current chunk; earlier chunks have fewer columns. Always bounds-check `col_index < chunk->columns.size()`. See `docs/USER_GUIDE.md §6` / `docs/REQUIREMENTS.md §4.5`.
- **`readString()` returns a `string_view` into chunk dictionary memory** — must not outlive the chunk.
- **Transforms have a strict sequential contract**: `calculate()` is called in ascending timestamp order; state persists across chunks and is cleared only by `reset()` before a batch recompute. A late (out-of-order) input commit therefore resets + fully replays the node. See `include/pj_datastore/derived_engine.hpp`.
- **Out-of-order ingest is lossless**: appends accept timestamp regressions; rows sort per chunk at seal, and sealed chunks of a topic may overlap in time. Row cursors (`forEach`, `SeriesCursor`) merge to global timestamp order; `forEachChunk` bulk runs arrive in commit order and may overlap. See `docs/ARCHITECTURE.md` §5. (`DataEngine::flushTo` / `ObjectStore::flushTo` still enforce monotonicity at the streaming swap boundary — tracked follow-up.)
- **`ObjectStore` is independent storage** alongside `DataEngine`, with its own mutex-per-series threading and lazy/owned payloads — it is NOT covered by `ARCHITECTURE.md`; read `docs/OBJECT_STORE_DESIGN.md`.

## Read deeper
| For | Read |
|---|---|
| What it must do / data model / schema-evolution contract | `docs/REQUIREMENTS.md` |
| How the scalar engine works (domain model, layers, encoding, DerivedEngine, data flow) | `docs/ARCHITECTURE.md` |
| Plugin-author write/read patterns, ValueRef, pitfalls | `docs/USER_GUIDE.md` |
| Opaque timestamped blob storage (lazy/owned, retention, ABI bridge) | `docs/OBJECT_STORE_DESIGN.md` |
| Engine entry point + commit/flush cycle | `include/pj_datastore/engine.hpp` |
| Write / read facades | `include/pj_datastore/writer.hpp`, `reader.hpp` |
| Series / range / latest-at queries | `include/pj_datastore/query.hpp` |
| Transform interfaces + built-ins | `include/pj_datastore/derived_engine.hpp`, `builtin_transforms.hpp` |
| C-ABI host bridges (source/parser/toolbox, object surfaces) | `include/pj_datastore/plugin_data_host.hpp`, `docs/OBJECT_STORE_DESIGN.md` |
