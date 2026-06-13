#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_datastore/sequential_uid.hpp"
#include "pj_scene3d_widgets/passes/marker_render_pass.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

class MeshLoader;
class MeshLoadSet;
struct MeshLoadEntry;
class UrlFetcher;

// Concrete Scene3DLayer for a single visualization_msgs/MarkerArray-equivalent
// topic (canonical sdk::SceneEntities, type kSceneEntities). Renders two
// primitive families through two render passes:
//
// - **Markers** (cube/sphere/cylinder/arrow/line/triangle primitives): owns a
//   MarkerRenderPass and decodes the latest batch at/before the tracker time
//   from the ObjectStore. Mirrors PointCloudEntity's
//   "decode-from-store-at-tracker-time" lifecycle.
// - **Models** (ModelPrimitive — embedded glTF bytes or a file/http URL): owns
//   a MeshRenderPass plus an assimp MeshLoader. Model state is *stateful*: all
//   batches up to the tracker time are replayed into an id-keyed entity map
//   (replace-by-id, deletions, lifetime expiry, per SceneUpdate semantics) and
//   mesh bytes load asynchronously with signature-based caching. URL sources
//   are fetched asynchronously (UrlFetcher); DATA-SUPPLIED http(s) URLs are
//   consent-gated behind the QSettings bool "pj_scene3d/allow_remote_model_fetch"
//   (default OFF — a crafted dataset must not drive network egress), while
//   local file URLs / bare paths always work. Blocked or failed URLs surface
//   through remoteFetchNotice().
//
// Known divergence (follow-up): the marker path shows only the latest batch,
// so markers from earlier batches with different entity ids disappear, while
// their models persist. Unifying markers onto the stateful entity map changes
// shipped behavior and deserves its own review.
//
// Exposes a per-instance config widget with the viewer-side display overrides
// (opacity / color-override / wireframe). These are a per-topic preference, NOT
// marker data — the marker protocol has no such fields. Opacity and the color
// override also apply to model draws; wireframe is marker-only (MeshRenderPass
// has no edge mode).
class SceneEntitiesLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~SceneEntitiesLayer() override;

  // Scene3DLayer / ISceneLayer
  // Identity row for the dock's layer list (family "Markers").
  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  // First/last sample timestamps of the topic in the ObjectStore.
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  // The marker batch's source frame plus every distinct model-entity frame.
  [[nodiscard]] QStringList fallbackFrames() const override;
  // Frame of the latest decoded marker batch (first entity's frame_id).
  [[nodiscard]] QString sourceFrame() const override;
  // Persist / restore the viewer-side display overrides (tag "markers").
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;

  // Bind to the session: resolve parser + time range, decode the first batch,
  // and replay the initial model state. Safe to call before any GL init.
  bool attach(const PJ::SceneLayerContext& ctx) override;
  // Drop parser/session bindings and all decoded marker + model state.
  void detach() override;

  // Re-decode markers and re-replay model state at the new playhead (no-op
  // while hidden; the next un-hide refreshes both paths).
  void setTrackerTime(PJ::Timepoint time) override;
  // Toggle both render paths; un-hiding re-decodes at the current playhead.
  void setVisible(bool visible) override;

  // GL lifecycle for both passes (per-context resources; see module CLAUDE.md).
  void initializeGL() override;
  // Draw markers, then drain finished mesh loads and draw model primitives.
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;

  // Opacity / color-override / wireframe controls bound to this instance.
  QWidget* createConfigWidget(QWidget* parent) override;

  // Per-instance display-override accessors used by the config widget. Kept on
  // the concrete class so the abstract base doesn't carry marker concepts.
  [[nodiscard]] float opacity() const {
    return overrides_.opacity;
  }
  [[nodiscard]] bool colorOverrideEnabled() const {
    return overrides_.color_override;
  }
  [[nodiscard]] QColor overrideColor() const {
    return QColor::fromRgbF(overrides_.override_color.r, overrides_.override_color.g, overrides_.override_color.b);
  }
  [[nodiscard]] bool wireframe() const {
    return overrides_.wireframe;
  }

  // Display-override setters: update overrides_, push them to the passes, and
  // request a repaint. Opacity ∈ [0,1] multiplies every primitive's alpha.
  void setOpacity(float opacity);
  void setColorOverrideEnabled(bool enabled);
  void setOverrideColor(QColor color);
  void setWireframe(bool enabled);

  // Model-path introspection for tests and diagnostics: the id-keyed entity map
  // accumulated at the last rebuilt tracker time, and the mesh draw calls the
  // layer would submit for a frame (fixed-frame TF × entity pose × scale).
  [[nodiscard]] const std::map<std::string, PJ::sdk::SceneEntity>& currentEntities() const {
    return entities_;
  }
  [[nodiscard]] std::vector<MeshRenderPass::DrawCall> modelDrawCallsForFrame(const FrameContext& frame_ctx) const;

  // Human-readable status of the model-URL fetch path (URLs blocked by the
  // remote-fetch consent gate, or failed fetches); empty when there is nothing
  // to surface. Shown by the config widget; remoteFetchNoticeChanged tracks it.
  [[nodiscard]] QString remoteFetchNotice() const {
    return remote_fetch_notice_;
  }

 signals:
  // remoteFetchNotice() changed (possibly back to empty).
  void remoteFetchNoticeChanged(const QString& notice);

 private:
  // Decode the first sample once at attach so we know the source frame and time
  // range before render is called.
  bool bootstrap();
  // Reset every replay/bootstrap artifact — decoded entities, the active marker
  // batch in pass_, replay cursors (last_marker_uid_ / last_applied_uid_ /
  // state_built_at_), snapshot cache, source-frame/time-range bookkeeping, mesh
  // loads — to the post-construction baseline. attach() runs it because a
  // dataset reload (SessionManager::replaceDataset) re-attaches a layer WITHOUT
  // an intervening detach(): prior-generation UIDs/timestamps must never skip or
  // anchor the new generation's replay, and a stale marker batch must not keep
  // drawing. detach() shares it for its state teardown.
  void resetReplayState();
  // Decode + push the batch at/before time_ns into the render pass. No-op when
  // there's no parser or the store has no sample at/before time_ns.
  void renderAt(int64_t time_ns);
  // Re-decode at the current playhead after a visibility change.
  void refreshNow();
  // Push the current overrides to the pass and request a repaint.
  void applyOverrides();

  // Model path: full replay of every batch up to `time` into entities_ (clears
  // first; replace-by-id, deletions, lifetime expiry), then kick async mesh loads
  // for the survivors. O(history) — used for the first build and backward scrubs.
  void rebuildModelStateAt(PJ::Timepoint time);
  // Bring entities_ to `time`. Skips when unchanged; folds only newly-appended
  // batches for forward playback (O(delta)); falls back to rebuildModelStateAt for
  // the first build, backward scrubs, or jumps. This keeps sustained playback from
  // re-parsing (and re-hashing heavy embedded models in) the whole history per frame.
  void ensureModelStateAt(PJ::Timepoint time);
  // Parse+fold ObjectStore entries with SequentialUID in (after_uid, target_uid]
  // into entities_ via applySnapshot, stepping the topic's sparse UID sequence
  // with nextUIDAfter (cache-first, parse on miss). A default/invalid after_uid
  // starts from the first retained entry. Returns true if a snapshot was applied.
  bool applyEntriesAfter(PJ::SequentialUID after_uid, PJ::SequentialUID target_uid);
  // Fold one decoded batch into entities_: apply the batch's deletions FIRST
  // (against the pre-batch map), then upsert by entity id. Order matters:
  // the SDK contract says deletions remove PRIOR entities; applying them first
  // lets the DELETEALL+re-add republish pattern work (deletion and new entities
  // in the same batch at the same timestamp → deletions clear old, upserts add
  // new). kAll / kMatchingId branches are gated on the deletion timestamp.
  void applySnapshot(const PJ::sdk::SceneEntities& snapshot);
  // Erase entities whose lifetime elapsed before `time`. Returns true when at
  // least one entity was erased (the caller owes a repaint).
  bool dropExpiredEntities(PJ::Timepoint time);
  // Insert a decoded batch into snapshot_cache_, tracking the byte estimate and
  // evicting lowest-UID batches once the budget is exceeded. No-op when the UID
  // is invalid or already cached.
  void cacheSnapshot(PJ::SequentialUID uid, std::shared_ptr<const PJ::sdk::SceneEntities> batch);
  // Drop cached batches below the store's first retained UID (retention pruning).
  void pruneSnapshotCacheBelow(PJ::SequentialUID first_retained_uid);
  // Recompute the distinct entity frames feeding fallbackFrames().
  void updateModelFrames();
  // Kick startMeshLoadIfNeeded for every model of every live entity.
  void startMeshLoadsForCurrentEntities();
  // Start an async mesh load for `key` unless its source bytes (signature) are
  // already loaded, loading, fetching, or recorded as blocked/failed; a changed
  // signature blanks the mesh and reloads. URL sources go through url_fetcher_
  // (never synchronously); remote http(s) URLs are consent-gated (see class doc).
  void startMeshLoadIfNeeded(const std::string& key, const PJ::sdk::ModelPrimitive& primitive);
  // Kick the assimp import of `bytes` on the entry: creates the future and the
  // #PR183 completion watcher that drives pollMeshLoads -> repaintRequested.
  void startRecordImport(MeshLoadEntry& entry, const QByteArray& bytes, const QString& format_hint);
  // Recompute remote_fetch_notice_ from the current records and emit
  // remoteFetchNoticeChanged when the text actually changed.
  void updateRemoteFetchNotice();
  // Drain finished async loads into the mesh pass and request a repaint when
  // anything landed. Called from render() and from each load's QFutureWatcher,
  // so a completed mesh shows up without waiting for an unrelated repaint.
  void pollMeshLoads();

  PJ::ObjectTopicId topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // Parsers are deliberately NOT cached: every decode resolves a fresh
  // ParserBinding through ctx_.session (see parseLocked()), so a file reload
  // that re-registers the topic's parser slot can never leave us dangling.

  std::string source_frame_;
  // The latest tracker time pushed to this entity; the time the cached batch was
  // (re)decoded at. Used by refreshNow() to re-decode at the current playhead.
  PJ::Timepoint decoded_at_ns_{};
  // Identity of the marker batch last decoded by renderAt; lets it skip
  // re-decoding the same message while scrubbing within one message's time window.
  PJ::SequentialUID last_marker_uid_;

  bool visible_ = true;
  int64_t ts_first_ = 0;

  // Viewer-side display overrides pushed wholesale to the pass. The override
  // color lives here as a normalized vec4; the config widget derives a QColor
  // from it on demand (overrideColor()) — single source of truth.
  MarkerRenderPass::DisplayOverrides overrides_;

  MarkerRenderPass pass_;

  // Model path (ModelPrimitive meshes). entities_ is the replayed id-keyed
  // state at state_built_at_; model_frames_ caches the distinct entity frames
  // merged into fallbackFrames().
  std::optional<PJ::Timepoint> state_built_at_;
  // Highest ObjectStore SequentialUID already folded into entities_. UID 0 is the
  // invalid sentinel; real entries start at 1. Lets forward playback apply only
  // the (last_applied_uid_, target_uid] delta instead of replaying from 0.
  PJ::SequentialUID last_applied_uid_;
  // One decoded batch shared between the marker pass seed and the model fold.
  // `bytes` is the estimate recorded at insert, so eviction subtracts exactly
  // what was added.
  struct CachedSnapshot {
    std::shared_ptr<const PJ::sdk::SceneEntities> batch;
    std::size_t bytes = 0;
  };
  // Decoded-batch cache keyed by ObjectStore SequentialUID, so a full rebuild
  // (backward scrub / jump) re-folds retained entities WITHOUT re-parsing the
  // history. Ordered map: pruning below the store's first retained UID and
  // evicting oldest-first past kSnapshotCacheMaxBytes are both range operations.
  std::map<PJ::SequentialUID, CachedSnapshot> snapshot_cache_;
  // Estimated heap bytes held by snapshot_cache_ (dominated by embedded model
  // bytes and line/triangle geometry). File sessions never evict from the store,
  // so without this budget the cache would grow with the whole topic history.
  std::size_t snapshot_cache_bytes_ = 0;
  std::map<std::string, PJ::sdk::SceneEntity> entities_;
  QStringList model_frames_;
  std::unique_ptr<MeshLoader> mesh_loader_;
  std::unique_ptr<MeshRenderPass> mesh_pass_;
  // Async mesh loads keyed by meshKey() ("topic:entity:index"); each entry's
  // identity is the model source signature (a re-published model with new bytes
  // replaces the entry). Held by unique_ptr so this header can forward-declare
  // MeshLoadSet (its definition lives in the private src/).
  std::unique_ptr<MeshLoadSet> mesh_loads_;
  // Async fetcher for URL-sourced models, owned by the layer: destroying the
  // layer (or resetReplayState) aborts in-flight fetches and drops their
  // callbacks, so a completion can never touch a dead record list.
  std::unique_ptr<UrlFetcher> url_fetcher_;
  // Cached remoteFetchNotice() text (see accessor).
  QString remote_fetch_notice_;
};

}  // namespace pj::scene3d
