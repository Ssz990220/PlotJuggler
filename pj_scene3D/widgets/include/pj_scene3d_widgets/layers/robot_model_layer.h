// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_datastore/object_store.hpp"
#include "pj_scene3d_core/robot_model.h"
#include "pj_scene3d_core/tf/transform.h"  // StampedTransform (cached fixed-joint bridges)
// Needed in full (not forward-declared) because render() memoizes
// std::vector<MeshRenderPass::DrawCall> members — a nested type that requires
// the enclosing class to be complete. The header is public + self-contained
// (it pulls mesh_data.h, no private src/ include), exactly as the sibling
// scene_entities_layer.h already includes it.
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"

class QWidget;

namespace pj::scene3d {

class MeshLoader;
class MeshLoadSet;
class UrdfPackageResolver;
class UrlFetcher;

// Scene3DLayer that renders a robot's URDF (links, primitive geometry, and
// async-loaded meshes) posed by the live TF tree. Unlike the per-tick store
// layers it decodes its description at most once per source change and then
// latches, so it is not a hot path.
//
// Three source modes (SourceType):
//   - kTopic: a robot_description object topic in the ObjectStore, decoded
//     through the topic's MessageParser. The first sample is latched; while the
//     topic is still empty the layer waits and retries on each tracker tick.
//   - kFile:  a local URDF/XML file read directly.
//   - kUrl:   a file://, http://, or https:// URL fetched ASYNCHRONOUSLY:
//     loadFromCurrentSource() (attach, xmlLoadState, the config widget) only
//     kicks the fetch off and the model is applied when the bytes land. The
//     layer never blocks the GUI thread on the network.
//
// Parser lifetime (kTopic): like its sibling store layers, parsers are
// deliberately NOT cached. Every decode resolves a fresh ParserBinding through
// ctx_.session (see parseLocked()), so a file reload that re-registers the
// topic's parser slot can never leave us decoding through a freed handle.
//
// The layer is deliberately bounds-less: it contributes nothing to the camera's
// scene-fit AABB (it inherits Scene3DLayer::worldBounds()'s std::nullopt
// default). A robot is posed by the live TF tree, which the camera already
// frames through the other store-backed layers, so adding its links to the fit
// would pull the camera around as joints move.
class RobotModelLayer : public Scene3DLayer {
  Q_OBJECT
 public:
  enum class SourceType { kTopic, kFile, kUrl };
  // kVisual: render only <visual> geometry; kCollision: render only <collision>
  // geometry (through the collision opacity/visibility group). kAuto: a link's
  // <visual> geometry when it has any; a collision-only link is promoted to the
  // visuals group (rendered solid) ONLY when the WHOLE model has no visuals, so an
  // all-collision URDF renders solid instead of ghosting at the collision group's
  // default opacity (review L.21). In a MIXED model (some links have visuals), a
  // collision-only link is auxiliary geometry (e.g. self-collision capsules) and
  // renders in the COLLISION group so the Collision opacity/visibility toggle
  // controls it rather than overlaying the visuals as an unhideable solid.
  enum class DisplayMode { kAuto, kVisual, kCollision };
  enum class XmlLoadResult { kRestored, kDeferred, kInvalid };

  RobotModelLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent = nullptr);
  ~RobotModelLayer() override;

  [[nodiscard]] PJ::SceneLayerInfo info() const override;
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override;
  [[nodiscard]] QStringList fallbackFrames() const override;
  [[nodiscard]] QString sourceFrame() const override;
  QDomElement xmlSaveState(QDomDocument& doc) const override;
  bool xmlLoadState(const QDomElement& element) override;
  [[nodiscard]] XmlLoadResult xmlLoadStateResult(const QDomElement& element);

  bool attach(const PJ::SceneLayerContext& ctx) override;
  void detach() override;

  void setFixedFrame(const QString& frame) override;
  void setTrackerTime(PJ::Timepoint time) override;
  void setVisible(bool visible) override;

  void initializeGL() override;
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override;
  void releaseGL() override;
  // worldBounds(): inherits Scene3DLayer's std::nullopt default (this layer is
  // bounds-less by design — see the class doc-comment).
  // Shadow casting: the robot meshes DO cast. meshShadowBounds reports the visual
  // links' world AABB (so the light frustum encloses the robot the camera doesn't
  // frame); renderShadowCasters depth-draws those visual links. Collision hulls are
  // excluded from both (not casters).
  [[nodiscard]] std::optional<AABB> meshShadowBounds(const FrameContext& frame_ctx) override;
  void renderShadowCasters(const glm::mat4& light_view_proj, const FrameContext& frame_ctx) override;

  QWidget* createConfigWidget(QWidget* parent) override;

  void setPackageResolver(UrdfPackageResolver* resolver);
  void setSourceTopic(PJ::ObjectTopicId topic_id, QString display_name = {});
  void setSourceFile(QString path);
  void setSourceUrl(QString url);
  void setFramePrefix(QString prefix);
  void setDisplayMode(DisplayMode mode);
  void setFallbackColor(QColor color);
  // Toggle the COLLADA up-axis override. When true, .dae/.collada meshes load
  // with the Y->Z flip forced OFF (the loader otherwise flips them — see
  // MeshLoader::load flip_override); other formats (STL/OBJ/glTF) are unaffected.
  // Changing the value reloads from the current source (clearing the loader
  // cache) so the new flip takes effect; an unchanged value is a no-op.
  void setIgnoreColladaUpAxis(bool ignore);

  [[nodiscard]] SourceType sourceType() const {
    return source_type_;
  }
  [[nodiscard]] QString sourceValue() const {
    return source_value_;
  }
  [[nodiscard]] QString framePrefix() const {
    return frame_prefix_;
  }
  [[nodiscard]] DisplayMode displayMode() const {
    return display_mode_;
  }
  [[nodiscard]] QColor fallbackColor() const {
    return fallback_color_;
  }
  [[nodiscard]] bool ignoreColladaUpAxis() const {
    return ignore_collada_up_axis_;
  }
  [[nodiscard]] QString statusText() const {
    return status_text_;
  }
  [[nodiscard]] const RobotModel* robotModel() const {
    return model_.has_value() ? &*model_ : nullptr;
  }
  [[nodiscard]] int totalMeshCount() const {
    return total_mesh_count_;
  }
  [[nodiscard]] int unresolvedMeshCount() const {
    return unresolved_mesh_count_;
  }
  [[nodiscard]] QString linkFrameName(const std::string& link_name) const;

#ifdef PJ_SCENE3D_TEST_HOOKS
  // Exposes the render() memoization dirty flag so a GL-less test can assert the
  // invalidation set: setters that change geometry flip it back on. Whether
  // render() consumes (clears) it cannot be exercised here — that path needs a
  // live GL context the unit harness does not provide, so the test clears the
  // flag manually (mimicking what render() does after a rebuild) and checks that
  // each invalidating call re-sets it.
  [[nodiscard]] bool drawsDirtyForTest() const {
    return draws_dirty_;
  }
  void clearDrawsDirtyForTest() {
    draws_dirty_ = false;
  }
  [[nodiscard]] bool drawCacheNeedsRebuildForTest(const FrameContext& frame_ctx) const {
    return drawCacheNeedsRebuild(frame_ctx);
  }
  // Runs the (GL-free) draw-cache rebuild — which injects the fixed-joint TF
  // bridges into frame_ctx.tf — so a test can assert the buffer the layer renders
  // against gets bridged, without standing up a GL context.
  void rebuildDrawCacheForTest(const FrameContext& frame_ctx) {
    rebuildDrawCache(frame_ctx);
  }
  // Draw-group sizes after the last rebuild — lets a GL-less test assert which
  // group (visuals vs collision) a link's geometry landed in.
  [[nodiscard]] int visualDrawCountForTest() const {
    return static_cast<int>(cached_visual_draws_.size());
  }
  [[nodiscard]] glm::vec3 firstVisualDrawTranslationForTest() const {
    return cached_visual_draws_.empty() ? glm::vec3{0.0f} : glm::vec3(cached_visual_draws_.front().model[3]);
  }
  [[nodiscard]] int collisionDrawCountForTest() const {
    return static_cast<int>(cached_collision_draws_.size());
  }
  [[nodiscard]] glm::vec4 firstCollisionDrawColorForTest() const {
    return cached_collision_draws_.empty() ? glm::vec4(0.0f) : cached_collision_draws_.front().color;
  }
#endif

 signals:
  // Mesh-load progress. UNIT: all three of loaded/total/unresolved count
  // per-GEOMETRY mesh references, NOT unique files — several links sharing one
  // mesh file each count once, and `loaded` reaches `total` once their shared
  // file is imported. (unresolved_packages, by contrast, is the resolver's
  // distinct-package list.) updateMeshCounters() is the single source of truth.
  void meshLoadStatusChanged(int loaded, int total, QStringList unresolved_packages);
  void unresolvedPackages(QStringList packages);
  void statusTextChanged(const QString& status);

 private:
  // Rebuild cached_visual_draws_ / cached_collision_draws_ from the model posed
  // by frame_ctx's TF lookups. Called by render() only when draws_dirty_.
  void rebuildDrawCache(const FrameContext& frame_ctx);
  // True when the memoized draw calls cannot be reused for this frame. The
  // matrices stored in the cache are in camera-relative render space, so the
  // cache is tied to the render origin as well as to TF/model state.
  [[nodiscard]] bool drawCacheNeedsRebuild(const FrameContext& frame_ctx) const;
  // Rebuild the draw cache iff drawCacheNeedsRebuild(), then clear draws_dirty_.
  // Shared by render() and the shadow hooks so a frame's shadow pre-pass and
  // color pass use one list built against the same render origin.
  void ensureDrawCache(const FrameContext& frame_ctx);
  // Recompute static_bridges_ from the current model_, with frame_prefix_ applied
  // to each parent/child frame. Called when the model loads or the prefix changes.
  void rebuildStaticBridges();
  // Inject any not-yet-present fixed-joint bridge into `buf`. Idempotent + guarded
  // (never overrides real /tf) and cheap, so rebuildDrawCache calls it every
  // rebuild against the LIVE render buffer (frame_ctx.tf) — which self-heals after
  // a buffer clear (same-file reload). No-op when static_bridges_ is empty.
  void ensureStaticBridges(TransformBuffer& buf);
  bool loadFromCurrentSource();
  bool tryLoadTopicDescription();
  bool applyRobotDescription(
      const QString& text, const QString& format, const QString& label, const QString& urdf_dir, bool source_is_url);
  void startMeshLoads();
  // Drain finished async loads into the mesh pass, recompute the loaded counter,
  // then emit meshLoadStatusChanged + repaintRequested when anything landed.
  // Called from render() and from each load's QFutureWatcher, so a completed
  // mesh replaces its placeholder without waiting for an unrelated repaint.
  void pollMeshLoads();
  // O(1): the resolved-path-keyed load finished successfully. Per-frame draw path.
  [[nodiscard]] bool meshReady(const std::string& key) const;
  // Single source of truth for all three counters: one traversal of every
  // per-geometry mesh reference. ++total per GeomMesh; ++unresolved when the
  // mesh is unresolved or has no resolved_path; ++loaded when its resolved path's
  // load is ready(). startMeshLoads() does NOT touch the counters.
  void updateMeshCounters();
  void setStatus(QString status);
  // Distinct unresolved package names for THIS model (not the dock-shared
  // resolver's global tally), in first-seen order. Empty when all resolved.
  [[nodiscard]] QStringList unresolvedPackagesList() const;
  // A short " • N http refs blocked / N absolute paths missing" suffix for the
  // status line, covering the unresolved kinds Locate cannot fix. Empty when
  // there are none.
  [[nodiscard]] QString unresolvedIssueClause() const;

  PJ::ObjectTopicId topic_id_;
  PJ::ObjectTopicId source_topic_id_;
  QString display_name_;
  Scene3DLayerContext ctx_;
  // No parser pointer is cached here: tryLoadTopicDescription() resolves a fresh
  // ParserBinding per decode (see the class lifetime note above).

  SourceType source_type_{SourceType::kTopic};
  QString source_value_;
  QString frame_prefix_;
  DisplayMode display_mode_{DisplayMode::kAuto};
  QColor fallback_color_{178, 178, 178};
  bool ignore_collada_up_axis_{false};
  bool visible_{true};
  QString fixed_frame_;
  QString status_text_;
  bool latch_pending_{false};
  std::chrono::steady_clock::time_point last_latch_retry_{};
  PJ::Timepoint tracker_time_{};

  std::optional<RobotModel> model_;
  // Cached fixed-joint TF bridges (frame_prefix_ already applied), rebuilt on
  // model load / prefix change. Re-asserted into ctx_.tf_buffer each tracker tick
  // by ensureStaticBridges() so links the data's /tf never places (e.g. a gripper
  // mount) still resolve. Empty when the model has no fixed joints.
  std::vector<StampedTransform> static_bridges_;
  // True iff any link has visual geometry; gates kAuto's collision-only promotion
  // (rebuildDrawCache). Cached at model load — it only depends on the latched model.
  bool model_has_visuals_{false};
  int total_mesh_count_{0};
  int unresolved_mesh_count_{0};
  int loaded_mesh_count_{0};

  // Memoized per-link DrawCall lists, rebuilt by render() only when their inputs
  // changed. The lists are view/projection-independent, but their model matrices
  // are in camera-relative render space (`FrameContext::lookup` subtracts
  // render_origin), so panning/zoom-to-cursor must rebuild them when the render
  // origin changes. The opacity/visibility gates and view_params stay per-frame.
  // INVALIDATION SET — must be kept exhaustive by construction. draws_dirty_ is
  // set in every place that can change the geometry: setTrackerTime (TF reaches
  // the layer ONLY via tracker ticks, which the dock drives on every live
  // ingest tick and every scrub, so TF-driven pose changes are covered — the
  // one residual is a TF mutation at an unchanged playhead with no tracker
  // tick, which renders one frame stale), setFixedFrame, setFramePrefix,
  // setDisplayMode, setFallbackColor, setIgnoreColladaUpAxis, setVisible(true)
  // (the cache may predate a hide), applyRobotDescription, xmlLoadState, detach,
  // and pollMeshLoads when a drain reported a change (a placeholder cube must
  // swap to the real mesh). When adding a new setter that affects geometry, add
  // it to this set.
  std::vector<MeshRenderPass::DrawCall> cached_visual_draws_;
  std::vector<MeshRenderPass::DrawCall> cached_collision_draws_;
  bool draws_dirty_{true};
  std::optional<glm::dvec3> cached_render_origin_;

  std::unique_ptr<UrdfPackageResolver> owned_resolver_;
  UrdfPackageResolver* resolver_{nullptr};
  std::unique_ptr<MeshLoader> mesh_loader_;
  std::unique_ptr<MeshRenderPass> mesh_pass_;
  // Async mesh loads keyed by resolved path. Held by unique_ptr so this header
  // can forward-declare MeshLoadSet (its definition lives in the private src/).
  std::unique_ptr<MeshLoadSet> mesh_loads_;
  // Async fetcher for the kUrl source, owned by the layer: destroying the layer
  // (or detach()) aborts an in-flight fetch and drops its callback.
  std::unique_ptr<UrlFetcher> url_fetcher_;
  // Monotonic stamp bumped by every loadFromCurrentSource()/detach(); a URL
  // fetch result is applied only if no newer load superseded it (source
  // switched, Retry pressed, layer detached).
  uint64_t url_fetch_generation_{0};
};

}  // namespace pj::scene3d
