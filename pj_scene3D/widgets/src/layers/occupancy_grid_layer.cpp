// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/layers/occupancy_grid_layer.h"

#include <QComboBox>
#include <QDomElement>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <any>
#include <memory>
#include <optional>

#include "pj_base/builtin/occupancy_grid.hpp"
#include "pj_base/builtin/occupancy_grid_update.hpp"
#include "pj_base/time.hpp"  // PJ::fromRaw, PJ::toRaw
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/camera/camera.h"  // AABB, occupancyGridBounds
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcOccGrid, "pj.scene3d.occupancy_grid")
}  // namespace

OccupancyGridLayer::OccupancyGridLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

OccupancyGridLayer::~OccupancyGridLayer() = default;

PJ::SceneLayerInfo OccupancyGridLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kOccupancyGrid,
      .display_name = display_name_,
      .family_name = QStringLiteral("OccupancyGrid"),
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> OccupancyGridLayer::timeRange() const {
  const auto* store = ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr;
  PJ::Range<PJ::Timepoint> range = PJ::liveTopicTimeRange(store, topic_id_);
  if (updates_topic_.has_value()) {
    // This is the one two-topic layer: in live mapping the OccupancyGridUpdate
    // sibling routinely extends past the last full keyframe, so union its range
    // in. Without it timeRange().max stops at the base topic, the dock's scrub
    // clamp pins the tracker short of the newest patch, and the grid freezes at
    // the last keyframe while updates keep arriving. Inverted-empty ranges from
    // liveTopicTimeRange() fold away cleanly under min/max.
    const auto updates = PJ::liveTopicTimeRange(store, *updates_topic_);
    range.min = std::min(range.min, updates.min);
    range.max = std::max(range.max, updates.max);
  }
  return range;
}

QStringList OccupancyGridLayer::fallbackFrames() const {
  if (source_frame_.empty()) {
    return {};
  }
  return {QString::fromStdString(source_frame_)};
}

QString OccupancyGridLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement OccupancyGridLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(QStringLiteral("occupancy_grid"));
  el.setAttribute(
      QStringLiteral("color_scheme"), color_scheme_ == OccupancyGridRenderPass::ColorScheme::kCostmap
                                          ? QStringLiteral("costmap")
                                          : QStringLiteral("map"));
  el.setAttribute(QStringLiteral("opacity"), static_cast<double>(opacity_));
  return el;
}

bool OccupancyGridLayer::xmlLoadState(const QDomElement& element) {
  color_scheme_ = element.attribute(QStringLiteral("color_scheme")) == QStringLiteral("costmap")
                      ? OccupancyGridRenderPass::ColorScheme::kCostmap
                      : OccupancyGridRenderPass::ColorScheme::kMap;
  bool ok = false;
  const float opacity = element.attribute(QStringLiteral("opacity"), QStringLiteral("0.7")).toFloat(&ok);
  if (ok) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  }
  grid_pass_.setColorScheme(color_scheme_);
  grid_pass_.setOpacity(opacity_);
  return true;
}

bool OccupancyGridLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcOccGrid) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!scene3d_ctx.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcOccGrid) << "attach: no parser for occupancy-grid topic" << topic_id_.id;
    return false;
  }

  // Discover the paired "<base>_updates" sibling in the same dataset (RViz
  // convention). Absent → base-only mode (each full grid is a keyframe).
  PJ::ObjectStore& store = scene3d_ctx.session->objectStore();
  const auto& desc = store.descriptor(topic_id_);
  updates_topic_ = store.findTopic(desc.dataset_id, desc.topic_name + "_updates");

  resetStreamingState();
  grid_pass_.setColorScheme(color_scheme_);
  grid_pass_.setOpacity(opacity_);
  // Streaming-tolerant attach, matching PointCloudLayer/SceneEntitiesLayer: a grid
  // topic can be attached before its first sample lands (layout restore at stream
  // start, catalog drag). bootstrap() only pre-warms source_frame_ from the first
  // keyframe — renderAt() self-heals it later — so a failure must NOT drop the
  // layer (SceneDockWidget::attach discards on false). renderAt no-ops on an empty
  // store, so we return true as long as session + parser binding exist.
  if (!bootstrap()) {
    qCWarning(lcOccGrid) << "attach: bootstrap failed for occupancy-grid topic" << topic_id_.id
                         << "— render will skip until a sample arrives";
  }
  return true;
}

void OccupancyGridLayer::detach() {
  grid_pass_.clearGrid();
  updates_topic_.reset();
  resetStreamingState();
}

void OccupancyGridLayer::resetStreamingState() {
  base_cache_.reset();
  base_cache_uid_ = {};
  updates_cursor_ = {};
  last_consumed_time_.reset();
  reconstructor_.invalidate();
}

bool OccupancyGridLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return false;
  }
  auto obj = parseLocked(binding, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcOccGrid) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = std::any_cast<PJ::sdk::OccupancyGrid>(&obj->object);
  if (grid == nullptr) {
    return false;
  }
  source_frame_ = grid->frame_id;

  if (!source_frame_.empty()) {
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

void OccupancyGridLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  // Per-tick binding snapshots, alive for the whole reconstructAt call below.
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  const auto updates_binding = updates_topic_.has_value() ? ctx_.session->parserBindingForObjectTopic(*updates_topic_)
                                                          : PJ::SessionManager::ParserBinding{};
  PJ::ObjectStore& store = ctx_.session->objectStore();

  // Retroactive-ingest check (must run BEFORE reconstructAt). The reconstructor's
  // forward path consumes only (last_t_, t] and assumes immutable history, but the
  // live drive follows the fastest topic's edge — an update can be ingested later
  // with ts <= the already-consumed time and would be skipped forever. Per-topic
  // entries are appended in non-decreasing ts order with process-globally
  // increasing UIDs, so checking the FIRST entry past the cursor suffices: if its
  // ts is at-or-before the high-water consumed time, the timeline changed
  // retroactively → discard all reconstructor state (snapshots may embed the
  // missed window too) and rebuild from base + full replay. Conservative: a
  // spurious invalidate only costs one full rebuild, never correctness.
  PJ::SequentialUID cursor_candidate = updates_cursor_;
  if (updates_topic_.has_value()) {
    // Candidate for the post-reconstruct cursor: the highest UID with ts <= t as
    // of NOW (UID order == ts order within a topic, so this is latestAt's entry).
    // Captured pre-reconstruct: entries racing in mid-reconstruct keep UIDs above
    // it and get re-examined (worst case re-applied via invalidate) next tick.
    if (const auto consumed = store.latestAt(*updates_topic_, time_ns); consumed.has_value()) {
      cursor_candidate = std::max(cursor_candidate, consumed->sequential_uid);
    }
    if (last_consumed_time_.has_value()) {
      const PJ::SequentialUID first_new = store.nextUIDAfter(*updates_topic_, updates_cursor_);
      if (first_new.valid()) {
        const auto first_entry = store.at(*updates_topic_, first_new);
        if (first_entry.has_value() && first_entry->timestamp <= *last_consumed_time_) {
          // Snapshot-preserving rewind instead of a from-base full rebuild: under
          // a latched base (one keyframe, then only updates) every late update on
          // a costmap whose stamps trail a faster topic's live edge would trigger
          // invalidate() + a full (base_ts, t] replay — cumulatively quadratic in
          // session length. invalidateAfter() keeps the snapshots strictly before
          // the late entry, rewinds last_t_ to the nearest one, and bounds the
          // replay to the window since first_entry->timestamp.
          reconstructor_.invalidateAfter(first_entry->timestamp);
          // The detector's frame of reference moves to this render's t (the cursor
          // below covers every entry with ts <= t). Keeping a stale high-water
          // could re-flag a not-yet-consumed entry every tick (rebuild loop).
          last_consumed_time_.reset();
        }
      }
    }
  }

  // base_at(t): the latest full grid with ts <= t, decoded to sdk::OccupancyGrid.
  // Memoized on the entry's SequentialUID: the common case is the same keyframe
  // tick after tick, and re-parsing deep-copies the full cell payload each time.
  auto base_at = [this, &store, &binding](PJ::Timestamp t) -> std::optional<PJ::sdk::OccupancyGrid> {
    auto entry = store.latestAt(topic_id_, t);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      return std::nullopt;
    }
    if (base_cache_.has_value() && entry->sequential_uid == base_cache_uid_) {
      return base_cache_;  // same store entry → reuse the parsed grid
    }
    auto obj = parseLocked(binding, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      return std::nullopt;
    }
    const auto* grid = std::any_cast<PJ::sdk::OccupancyGrid>(&obj->object);
    if (grid == nullptr) {
      return std::nullopt;
    }
    base_cache_uid_ = entry->sequential_uid;
    base_cache_ = *grid;  // copy carries the anchor → bytes stay alive past the call
    return base_cache_;
  };

  // updates_in(lo, hi): updates with lo < ts <= hi, ascending. Traverses the
  // topic by stable SequentialUID (the SceneEntitiesLayer::applyEntriesAfter
  // pattern): raw indices shift when the streaming import thread evicts from the
  // front mid-iteration, skipping or double-applying patches. UIDs are sparse
  // per topic — step only via nextUIDAfter, never by incrementing. UID order ==
  // ts order within a topic, so the walk can stop at the first entry past hi.
  auto updates_in = [this, &store, &updates_binding](
                        PJ::Timestamp lo, PJ::Timestamp hi) -> std::vector<PJ::sdk::OccupancyGridUpdate> {
    std::vector<PJ::sdk::OccupancyGridUpdate> out;
    if (!updates_topic_.has_value() || !updates_binding) {
      return out;
    }
    const PJ::ObjectTopicId id = *updates_topic_;
    // Boundary: the newest entry with ts <= lo; everything past its UID is the
    // (lo, ...] suffix. Nullopt (empty topic, or all entries past lo) starts the
    // walk from the first retained entry.
    const auto boundary = store.latestAt(id, lo);
    const PJ::SequentialUID start_after = boundary.has_value() ? boundary->sequential_uid : PJ::SequentialUID{};
    for (PJ::SequentialUID uid = store.nextUIDAfter(id, start_after); uid.valid(); uid = store.nextUIDAfter(id, uid)) {
      auto entry = store.at(id, uid);
      if (!entry.has_value()) {
        continue;  // evicted between the UID step and the resolve
      }
      if (entry->timestamp > hi) {
        break;  // ts is non-decreasing along the UID walk — past the window
      }
      if (entry->timestamp <= lo || entry->payload.bytes.empty()) {
        continue;  // equal-ts run appended at the boundary after the resolve
      }
      auto obj = parseLocked(updates_binding, entry->timestamp, entry->payload);
      if (!obj.has_value()) {
        continue;
      }
      const auto* update = std::any_cast<PJ::sdk::OccupancyGridUpdate>(&obj->object);
      if (update == nullptr) {
        continue;
      }
      out.push_back(*update);
    }
    return out;
  };

  const GridUpdate update = reconstructor_.reconstructAt(time_ns, base_at, updates_in);
  // Advance the retroactive-ingest cursor. Safe even on an Empty result: entries
  // it covers are either applied, or re-read in full by the epoch rebuild the
  // next successful reconstructAt performs (the window always restarts at the
  // base keyframe after an invalidate).
  updates_cursor_ = cursor_candidate;
  last_consumed_time_ =
      last_consumed_time_.has_value() ? std::max(*last_consumed_time_, PJ::Timestamp{time_ns}) : time_ns;

  const ReconstructedGrid& grid = update.grid;
  if (grid.empty()) {
    grid_pass_.clearGrid();
    return;
  }
  if (grid.frame_id != source_frame_) {
    source_frame_ = grid.frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  // Anything but an incremental forward step (new epoch / backward seek) re-uploads
  // the whole texture; an incremental update uploads just the changed rects. The
  // pass discards dirty_rects on a full rebuild, so only materialize the vector
  // (potentially large after a backward-seek replay) on the incremental path.
  const bool incremental = update.kind == GridUpdate::Kind::Incremental;
  grid_pass_.setGrid(
      grid, !incremental,
      incremental ? std::vector<CellRect>(update.dirty.begin(), update.dirty.end()) : std::vector<CellRect>{});
}

void OccupancyGridLayer::setFixedFrame(const QString& frame) {
  // The grid is frame-relative; the render pass places it per-frame via its
  // FrameContext lookup against the fixed frame, so no re-decode is needed —
  // just request a paint. The layer keeps no copy of the frame.
  Q_UNUSED(frame);
  emit repaintRequested();
}

void OccupancyGridLayer::setTrackerTime(PJ::Timepoint time) {
  Q_UNUSED(time);  // render() reads frame_ctx.time via the tracker_dirty_ path
  // Defer the reconstruction to render() — see tracker_dirty_. Also stops a
  // hidden layer from reconstructing on every tick (render() skips it instead).
  tracker_dirty_ = true;
  emit repaintRequested();
}

void OccupancyGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  grid_pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time, marking tracker_dirty_ for the next paint.
  emit repaintRequested();
}

void OccupancyGridLayer::initializeGL() {
  grid_pass_.initializeGL();
}

void OccupancyGridLayer::releaseGL() {
  grid_pass_.releaseGL();
}

void OccupancyGridLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  if (!visible_) {
    return;
  }
  // Drain a pending tracker move (one reconstruction per painted frame; the
  // reconstructor's incremental path keeps a forward step cheap). A layer hidden
  // during the move keeps the flag set and catches up on its first visible frame.
  if (tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  grid_pass_.render(view_params, frame_ctx);
}

std::optional<AABB> OccupancyGridLayer::worldBounds() const {
  // Bounds come from the grid as currently reconstructed (whatever the last
  // renderAt produced). Empty before any base keyframe has been seen.
  const ReconstructedGrid& grid = reconstructor_.grid();
  if (grid.empty()) {
    return std::nullopt;
  }
  const glm::vec3 origin{
      static_cast<float>(grid.origin.position.x), static_cast<float>(grid.origin.position.y),
      static_cast<float>(grid.origin.position.z)};
  const AABB box = occupancyGridBounds(origin, grid.resolution, grid.width, grid.height);
  if (!box.valid) {
    return std::nullopt;
  }
  return box;
}

void OccupancyGridLayer::setColorScheme(OccupancyGridRenderPass::ColorScheme scheme) {
  color_scheme_ = scheme;
  grid_pass_.setColorScheme(scheme);
  emit repaintRequested();
}

void OccupancyGridLayer::setOpacity(float opacity) {
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  grid_pass_.setOpacity(opacity_);
  emit repaintRequested();
}

QWidget* OccupancyGridLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(6);
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(6);
  outer->addLayout(form);

  auto* scheme_combo = new PJ::ComboBox(container);
  scheme_combo->addItem(tr("Map (grayscale)"), static_cast<int>(OccupancyGridRenderPass::ColorScheme::kMap));
  scheme_combo->addItem(tr("Costmap"), static_cast<int>(OccupancyGridRenderPass::ColorScheme::kCostmap));
  scheme_combo->setCurrentIndex(color_scheme_ == OccupancyGridRenderPass::ColorScheme::kCostmap ? 1 : 0);
  form->addRow(tr("Colors:"), scheme_combo);
  QObject::connect(scheme_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    setColorScheme(
        idx == 1 ? OccupancyGridRenderPass::ColorScheme::kCostmap : OccupancyGridRenderPass::ColorScheme::kMap);
  });

  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(static_cast<double>(opacity_));
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(
      opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(static_cast<float>(v)); });

  return container;
}

}  // namespace pj::scene3d
