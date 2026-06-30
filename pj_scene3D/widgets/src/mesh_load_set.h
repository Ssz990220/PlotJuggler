// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared async-mesh-load bookkeeping for the layers that drive MeshLoader on a
// background thread (RobotModelLayer, SceneEntitiesLayer). Both layers kick off
// QFuture<MeshData> imports, watch them with a per-load QFutureWatcher, and on
// completion drain the result into a MeshRenderPass + request a repaint. This
// helper owns that lifecycle once so the two layers stop duplicating the watcher
// wiring, the exception-guarded result() drain (the H.9 defense-in-depth
// barrier), and the completion bookkeeping.

#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "pj_scene3d_widgets/mesh_data.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"

namespace pj::scene3d {

// One async mesh load. `identity` lets a layer detect that the SAME map key now
// refers to different source bytes (SceneEntities re-publishes a ModelPrimitive
// with new content under a stable key) and replace this entry; RobotModelLayer
// keys by resolved path, so identity simply mirrors the key there. The watcher
// is owned by the entry so replacing/clearing the entry severs its finished
// connection — a stale completion can never fire on a dead record.
struct MeshLoadEntry {
  std::string identity;
  QFuture<MeshData> future;
  std::unique_ptr<QFutureWatcher<MeshData>> watcher;
  bool consumed{false};
  bool failed{false};
  // URL-fetch bookkeeping, set only by SceneEntitiesLayer's model-URL path (the
  // robot layer's URL source is single and surfaces through its own status):
  //   blocked_by_policy — a data-supplied remote URL refused by the policy gate.
  //   fetch_failed       — the URL fetch itself failed (vs. a failed assimp import).
  // Both feed remoteFetchNotice(); they stay false for embedded/local loads.
  bool blocked_by_policy{false};
  bool fetch_failed{false};
  // Human-readable status detail (SceneEntitiesLayer model path only):
  //   source_label — the model URL, or "(embedded model)" for inline bytes.
  //   error        — the failure detail (network error for fetch_failed; the
  //                  importer's message for an import failure). Empty when ok.
  // drain() fills `error` from MeshData::error on an import failure.
  QString source_label;
  QString error;
};

// Map of mesh-load entries keyed by the layer's lookup key (resolved path for
// RobotModelLayer; "topic:entity:index" for SceneEntitiesLayer). GUI-thread only
// — the watchers fire on the thread that armed them, and the map is mutated from
// render()/setTrackerTime() and the watcher callbacks, all on the GUI thread.
class MeshLoadSet {
 public:
  // Result of draining finished loads: `changed` is true when at least one entry
  // transitioned to consumed this call (callers refresh derived state on that).
  struct DrainResult {
    bool changed{false};
  };

  // Per-entry callback invoked by drain() when a load completed but FAILED
  // (assimp returned ok=false, or result() threw). RobotModelLayer uses it to
  // evict the failed path from MeshLoader so an explicit Retry re-imports
  // instead of replaying the cached failure. `key` is the map key; `entry` is
  // the just-failed entry (already marked consumed+failed).
  using FailureCallback = std::function<void(const std::string& key, const MeshLoadEntry& entry)>;

  [[nodiscard]] MeshLoadEntry* find(const std::string& key) {
    auto it = entries_.find(key);
    return it != entries_.end() ? &it->second : nullptr;
  }
  [[nodiscard]] const MeshLoadEntry* find(const std::string& key) const {
    auto it = entries_.find(key);
    return it != entries_.end() ? &it->second : nullptr;
  }

  // Insert a fresh entry for `key` (replacing any existing one — which destroys
  // the old watcher and severs its connection) and stamp its identity. The
  // caller fills in future/watcher via arm() after kicking the load.
  MeshLoadEntry& insertOrReplace(const std::string& key, std::string identity) {
    MeshLoadEntry& entry = entries_[key];  // value-inits or replaces in place
    entry = MeshLoadEntry{};
    entry.identity = std::move(identity);
    return entry;
  }

  // Create the entry's watcher and connect its finished signal to `on_finished`,
  // with `context` as the connection context so the connection dies with the
  // context object. Connect happens BEFORE setFuture so an already-finished load
  // (MeshLoader cache hit) still fires the slot. `entry.future` must be set first.
  void arm(MeshLoadEntry& entry, QObject* context, std::function<void()> on_finished) {
    entry.watcher = std::make_unique<QFutureWatcher<MeshData>>();
    // The watcher lives on this (GUI) thread; connect BEFORE setFuture so an
    // already-finished load (cache hit) still signals.
    QObject::connect(entry.watcher.get(), &QFutureWatcher<MeshData>::finished, context, std::move(on_finished));
    entry.watcher->setFuture(entry.future);
  }

  // Drain every finished, not-yet-consumed entry that carries a valid future:
  // exception-guarded future.result() (the single H.9 defense-in-depth barrier),
  // push successful MeshData into `pass`, mark the entry consumed/failed, and on
  // failure invoke `on_failure` (if provided). Entries with an invalid future
  // (a SceneEntities URL record still awaiting its bytes) are skipped.
  DrainResult drain(MeshRenderPass& pass, const FailureCallback& on_failure = {}) {
    DrainResult result;
    for (auto& [key, entry] : entries_) {
      // A default-constructed QFuture reports finished but holds no result —
      // isValid() filters it (a pending URL fetch leaves the future invalid).
      if (entry.consumed || !entry.future.isValid() || !entry.future.isFinished()) {
        continue;
      }
      MeshData data;
      // Defense in depth: result() rethrows any exception the worker stored. The
      // import has its own barrier (MeshLoader), but a throw escaping here would
      // unwind through paintGL and terminate the app.
      try {
        data = entry.future.result();
      } catch (const std::exception& ex) {
        data.error = QStringLiteral("mesh load threw: %1").arg(QString::fromUtf8(ex.what()));
      } catch (...) {
        data.error = QStringLiteral("mesh load threw an unknown exception");
      }
      entry.failed = !data.ok;
      if (data.ok) {
        pass.setMeshData(key, std::move(data));
      } else {
        entry.error = data.error;  // surfaced by SceneEntitiesLayer's load-failure notice
        if (on_failure) {
          on_failure(key, entry);
        }
      }
      entry.consumed = true;
      result.changed = true;
    }
    return result;
  }

  // True once `key`'s load finished successfully (consumed && !failed) — the
  // O(1) draw-path readiness check.
  [[nodiscard]] bool ready(const std::string& key) const {
    const MeshLoadEntry* entry = find(key);
    return entry != nullptr && entry->consumed && !entry->failed;
  }

  // Drop one entry by key (destroys its watcher, severing any pending
  // completion). Used when the owning entity is deleted/expires so a stale
  // failure record stops feeding the status notice. No-op if absent.
  void erase(const std::string& key) {
    entries_.erase(key);
  }

  void clear() {
    entries_.clear();
  }

  // Iteration access for callers that need to scan all entries (e.g.
  // SceneEntitiesLayer's remote-fetch-notice tally over per-entry flags it owns
  // through the entry's identity — see that layer's record wrapper).
  [[nodiscard]] auto begin() {
    return entries_.begin();
  }
  [[nodiscard]] auto end() {
    return entries_.end();
  }
  [[nodiscard]] auto begin() const {
    return entries_.begin();
  }
  [[nodiscard]] auto end() const {
    return entries_.end();
  }

 private:
  std::unordered_map<std::string, MeshLoadEntry> entries_;
};

}  // namespace pj::scene3d
