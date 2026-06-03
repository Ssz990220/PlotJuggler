// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/Scene3DDockWidget.h"

#include <QAbstractItemView>
#include <QBoxLayout>
#include <QFontMetrics>
#include <QLoggingCategory>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <unordered_set>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tracker_time.h"
#include "pj_widgets/ComboBox.h"
// Concrete entity headers — each new drawable kind adds its include
// here and a branch in addTopic's switch. Future work (post-v1) may
// replace this with a Scene3DEntityFactory registry to support
// plugin-provided drawables; until then, a switch is enough.
#include "pj_scene3d_widgets/entities/occupancy_grid_entity.h"
#include "pj_scene3d_widgets/entities/pointcloud_entity.h"
#include "pj_scene3d_widgets/scene3d_entity.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"

namespace PJ {

namespace {
Q_LOGGING_CATEGORY(lcScene3DDock, "pj.scene3d.dock")

using pj::scene3d::FrameRow;
using pj::scene3d::OccupancyGridEntity;
using pj::scene3d::PointCloudEntity;
using pj::scene3d::Scene3DEntity;
using pj::scene3d::Scene3DEntityContext;
using pj::scene3d::TransformBuffer;

bool framesContain(const QList<FrameRow>& frames, const QString& name) {
  const auto needle = name.toStdString();
  return std::any_of(frames.begin(), frames.end(), [&](const FrameRow& r) { return r.name == needle; });
}

// Case-insensitive less for frame names — mirrors tinytf's display-order
// comparator. Used when interleaving entity-fallback orphan roots with the
// already-sorted TF forest so the user sees one consistent alphabetical view.
bool frameNameLess(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) < std::tolower(y);
  });
}

QString pickFixedFrame(const QList<FrameRow>& frames) {
  for (const auto* name : {"map", "world", "odom", "base_link", "base_footprint"}) {
    if (framesContain(frames, QString::fromLatin1(name))) {
      return QString::fromLatin1(name);
    }
  }
  return frames.isEmpty() ? QString() : QString::fromStdString(frames.first().name);
}

}  // namespace

Scene3DDockWidget::Scene3DDockWidget(QWidget* parent) : QWidget(parent) {
  setWindowTitle(tr("3D View"));
  // Belt-and-braces zero margins: QSS `QWidget { padding: 0px; }` doesn't
  // touch widgets whose styled-background flag isn't set, so set the
  // contents margins explicitly on both the dock and its child view.
  setContentsMargins(0, 0, 0, 0);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  view_ = new pj::scene3d::SceneViewWidget(this);
  view_->setContentsMargins(0, 0, 0, 0);
  view_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(view_);

  connect(view_, &pj::scene3d::SceneViewWidget::framesChanged, this, &Scene3DDockWidget::onAvailableFrames);

  // Floating fixed-frame combo overlaid on the top-left of view_. A PJ::ComboBox
  // (the app-wide themed dropdown) so it tracks the light/dark stylesheet and
  // gets the same popup styling as every other combo — no per-call-site QSS.
  // Created as a sibling-child of `this` (not parented to view_) so Qt composites
  // it on top of the QOpenGLWidget without the well-known QOpenGLWidget
  // child-widget z-order glitches. Positioned manually in resizeEvent.
  //
  // Width is computed per-selection from the *currently selected* item's
  // text (see layoutFrameOverlayCombo) rather than sized to the longest
  // item — that keeps the overlay compact even when one frame name in the
  // dropdown is very long. Dropdown popup width is widened separately so
  // every row is fully readable when expanded.
  frame_overlay_combo_ = new ComboBox(this);
  frame_overlay_combo_->setFocusPolicy(Qt::ClickFocus);
  frame_overlay_combo_->raise();
  refreshFrameOverlayCombo();

  connect(frame_overlay_combo_, &QComboBox::currentIndexChanged, this, &Scene3DDockWidget::onOverlayFramePicked);
}

Scene3DDockWidget::~Scene3DDockWidget() {
  // Detach entities before they're destroyed so any view-level state
  // (registration in SceneViewWidget) is unwound first.
  for (auto& [key, entity] : entities_) {
    if (entity != nullptr) {
      view_->removeEntity(entity.get());
      entity->detach();
    }
  }
}

void Scene3DDockWidget::setSessionManager(SessionManager* session) {
  session_ = session;
}

void Scene3DDockWidget::setTransformService(pj::scene3d::TransformService* service) {
  transform_service_ = service;
}

void Scene3DDockWidget::setThemeHint(const QString& theme) {
  if (view_ != nullptr) {
    view_->setThemeHint(theme);
  }
}

bool Scene3DDockWidget::tryAcceptObjectTopic(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  // Family pre-check so DockWidget's replacement-fallback path doesn't
  // trip the "unsupported object_type" warning inside addTopic for
  // drops we expect to refuse (e.g. an Image dropped onto a 3D widget
  // — the host catches the false return and constructs a Media2D).
  if (object_type != sdk::BuiltinObjectType::kPointCloud && object_type != sdk::BuiltinObjectType::kFrameTransforms &&
      object_type != sdk::BuiltinObjectType::kOccupancyGrid) {
    return false;
  }
  return addTopic(topic_id, object_type, title);
}

bool Scene3DDockWidget::addTopic(ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& title) {
  if (session_ == nullptr) {
    qCWarning(lcScene3DDock) << "addTopic: session is null";
    return false;
  }
  if (entities_.contains(topic_id.id)) {
    return true;  // idempotent
  }

  // First-topic binding: pull the per-dataset TF buffer. Subsequent
  // topic additions inherit the same buffer (multi-topic is single-
  // dataset for now — cross-dataset is out of scope).
  ObjectStore& store = session_->objectStore();
  const auto dataset_id = store.descriptor(topic_id).dataset_id;
  if (!tf_buffer_ && transform_service_ != nullptr) {
    tf_buffer_ = transform_service_->transformBuffer(dataset_id);
    view_->setTransformBuffer(tf_buffer_);
    // The TF buffer just bound: announce TF as a permanent display so the side
    // panel adds its row even for a /tf-only drop (which emits no entityAdded).
    if (tfPresent()) {
      emit tfPresenceChanged(true);
    }
  }

  // TF is dataset-wide. Dropping a /tf topic by itself binds the TF
  // buffer above and renders the axes — no per-topic entity needed.
  if (object_type == sdk::BuiltinObjectType::kFrameTransforms) {
    setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
    return true;
  }

  // Construct the right concrete entity for the family. Each new
  // drawable kind adds a branch here; a registry replaces this only
  // when plugin-provided drawables land (post-v1).
  std::unique_ptr<Scene3DEntity> entity;
  const QString display_name = title.isEmpty() ? tr("(unnamed)") : title;
  switch (object_type) {
    case sdk::BuiltinObjectType::kPointCloud:
      entity = std::make_unique<PointCloudEntity>(topic_id, display_name, this);
      break;
    case sdk::BuiltinObjectType::kOccupancyGrid:
      entity = std::make_unique<OccupancyGridEntity>(topic_id, display_name, this);
      break;
    default:
      qCWarning(lcScene3DDock) << "addTopic: unsupported object_type" << static_cast<int>(object_type);
      return false;
  }

  Scene3DEntity* entity_raw = entity.get();
  Scene3DEntityContext ctx{session_, tf_buffer_};
  if (!entity->attach(ctx)) {
    qCWarning(lcScene3DDock) << "addTopic: entity attach failed for topic_id=" << topic_id.id;
    return false;
  }

  // Register with the view before bookkeeping so the first paint after
  // attach has a registered entity to iterate.
  view_->addEntity(entity_raw);
  absorbFallbackFrames(entity_raw);
  if (!view_->fixedFrame().empty()) {
    entity->setFixedFrame(QString::fromStdString(view_->fixedFrame()));
  }

  // Bridge per-entity events to the dock's generic signals. The entity
  // owns the connections (Qt parent), so they're cleaned up
  // automatically on entity destruction.
  connect(entity_raw, &Scene3DEntity::visibilityChanged, this, [this, topic_id](bool visible) {
    emit entityVisibilityChanged(topic_id, visible);
  });
  connect(entity_raw, &Scene3DEntity::fallbackFramesChanged, this, [this, entity_raw](const QStringList&) {
    absorbFallbackFrames(entity_raw);
  });
  connect(entity_raw, &Scene3DEntity::sourceFrameChanged, this, [this](const QString&) { recomputeOrphanStates(); });
  connect(entity_raw, &Scene3DEntity::repaintRequested, view_, [this]() { view_->update(); });

  // Seed the entity at the current tracker time so it reconstructs and stages
  // its grid for the imminent repaint (addEntity already requested one), rather
  // than rendering empty until the next playback tick. Done after the
  // repaintRequested connect so the entity's own update request is honored too.
  // Seed the entity so it shows immediately, clamped to the entity's OWN range.
  // The slider minimum can sit in a "dead zone" before a dynamic topic's first
  // sample — the window-fallback pins the minimum to a clamped latched topic
  // (e.g. /map_amcl) while a costmap's data starts a fraction of a second later.
  // Without the clamp the new entity reconstructs empty at the playhead and
  // stays blank until the user scrubs past the gap. Clamping shows its nearest
  // (first) frame now; onTrackerTime tracks the true playhead during playback.
  if (entity_raw->info().visible) {
    const auto [first, last] = entity_raw->timeRangeNs();
    const int64_t seed_ns = (last >= first) ? std::clamp(last_tracker_ns_, first, last) : last_tracker_ns_;
    entity_raw->setTrackerTime(std::chrono::nanoseconds{seed_ns});
  }

  entities_.emplace(topic_id.id, std::move(entity));
  emit entityAdded(topic_id);
  recomputeOrphanStates();

  setWindowTitle(title.isEmpty() ? tr("3D View") : tr("3D View - %1").arg(title));
  return true;
}

void Scene3DDockWidget::removeTopic(ObjectTopicId topic_id) {
  auto it = entities_.find(topic_id.id);
  if (it == entities_.end()) {
    return;
  }
  if (it->second != nullptr) {
    view_->removeEntity(it->second.get());
    // Free the entity's GL resources with the view's context current. Otherwise
    // its render-pass wrapper destructors run with no current context and the
    // gl wrappers self-skip glDelete, leaking the VBO/VAO/texture into the live
    // context until the whole view is torn down.
    view_->makeCurrent();
    it->second->releaseGL();
    view_->doneCurrent();
    it->second->detach();
  }
  entities_.erase(it);
  orphan_states_.erase(topic_id.id);
  emit entityRemoved(topic_id);
}

void Scene3DDockWidget::onTrackerTime(double time) {
  constexpr double kNsPerSec = 1.0e9;
  const auto raw_ns = static_cast<int64_t>(time * kNsPerSec);
  const auto ts_ns = clampToEntityRange(raw_ns);
  last_tracker_ns_ = ts_ns;
  view_->setTrackerTime(std::chrono::nanoseconds{ts_ns});
  for (auto& [key, entity] : entities_) {
    if (entity != nullptr && entity->info().visible) {
      entity->setTrackerTime(std::chrono::nanoseconds{ts_ns});
    }
  }
}

int64_t Scene3DDockWidget::clampToEntityRange(int64_t time_ns) const {
  // Collect each entity's [first, last] and defer to the pure, tested clamp.
  // The subtlety it gets right: a latched/one-shot grid (zero-span range, e.g.
  // /map pinned to the recording start) lowers the lower bound — so the slider
  // minimum snaps onto its exact ns — but must NOT cap the upper bound, or its
  // lone early timestamp would drag the live playhead backwards and hide
  // everything keyed to "now" (TF axes, live grids). See clampTrackerTimeToRanges
  // and tracker_time_test.cpp.
  std::vector<pj::scene3d::EntityTimeRange> ranges;
  ranges.reserve(entities_.size());
  for (const auto& [key, entity] : entities_) {
    if (entity == nullptr) {
      continue;
    }
    const auto [first, last] = entity->timeRangeNs();
    ranges.push_back({first, last});
  }
  return pj::scene3d::clampTrackerTimeToRanges(time_ns, ranges);
}

void Scene3DDockWidget::absorbFallbackFrames(Scene3DEntity* entity) {
  if (entity == nullptr) {
    return;
  }
  bool changed = false;
  for (const QString& f : entity->fallbackFrames()) {
    const std::string s = f.toStdString();
    if (std::find(fallback_frames_.begin(), fallback_frames_.end(), s) == fallback_frames_.end()) {
      fallback_frames_.push_back(s);
      changed = true;
    }
  }
  if (changed) {
    onAvailableFrames(available_frames_);
  }
}

void Scene3DDockWidget::onAvailableFrames(const QList<FrameRow>& frames) {
  // Group the TF hierarchy into (root, subtree) clusters — DFS pre-order
  // means every depth-0 row starts a contiguous cluster of its descendants.
  // Then append entity-fallback frames as their own single-row clusters
  // (orphans, depth 0) and sort clusters by root name. The result is a
  // valid DFS pre-order with fallbacks interleaved alphabetically among
  // real roots.
  std::vector<QList<FrameRow>> clusters;
  for (const auto& row : frames) {
    if (row.depth == 0 || clusters.empty()) {
      clusters.emplace_back();
    }
    clusters.back().append(row);
  }
  for (const auto& fb : fallback_frames_) {
    if (!framesContain(frames, QString::fromStdString(fb))) {
      clusters.push_back({FrameRow{fb, 0}});
    }
  }
  std::sort(clusters.begin(), clusters.end(), [](const QList<FrameRow>& a, const QList<FrameRow>& b) {
    return frameNameLess(a.first().name, b.first().name);
  });

  QList<FrameRow> effective;
  for (auto& cluster : clusters) {
    for (auto& row : cluster) {
      effective.append(std::move(row));
    }
  }

  if (effective.isEmpty()) {
    return;
  }
  available_frames_ = effective;
  emit availableFramesChanged(effective);
  refreshFrameOverlayCombo();

  // AutoRoot mode keeps re-resolving on every TF arrival; Explicit mode only
  // bootstraps when no frame has been chosen yet.
  if (fixed_frame_mode_ == FixedFrameMode::kAutoRoot || currentFixedFrame().isEmpty()) {
    applyResolvedFixedFrame(pickFixedFrame(effective));
  }
  // TF topology changed — re-check every entity even when the resolved fixed
  // frame is unchanged. A new edge can connect a previously-orphan entity,
  // or vice versa.
  recomputeOrphanStates();
}

QString Scene3DDockWidget::currentFixedFrame() const {
  if (view_ == nullptr) {
    return {};
  }
  return QString::fromStdString(view_->fixedFrame());
}

std::vector<Scene3DDockWidget::TopicInfo> Scene3DDockWidget::entities() const {
  // Return in render order (the view's entity vector), NOT entities_ map order
  // (which is arbitrary hash order). This keeps the Topics list in draw order
  // and puts newly added entities — appended to the view via push_back — at the
  // bottom, matching the drag-reorder convention (top = behind, bottom = on top).
  std::vector<TopicInfo> out;
  if (view_ == nullptr) {
    return out;
  }
  out.reserve(view_->entities().size());
  for (const pj::scene3d::Scene3DEntity* entity : view_->entities()) {
    if (entity == nullptr) {
      continue;
    }
    const auto info = entity->info();
    out.push_back(TopicInfo{info.topic_id, info.display_name, info.visible});
  }
  return out;
}

bool Scene3DDockWidget::topicVisible(ObjectTopicId topic_id) const {
  auto it = entities_.find(topic_id.id);
  return it != entities_.end() && it->second != nullptr && it->second->info().visible;
}

bool Scene3DDockWidget::tfPresent() const {
  return tf_buffer_ != nullptr && !tf_buffer_->getFrameHierarchy().empty();
}

bool Scene3DDockWidget::tfVisible() const {
  return view_ != nullptr && view_->axesVisible();
}

void Scene3DDockWidget::setTfVisible(bool visible) {
  if (view_ != nullptr) {
    view_->setAxesVisible(visible);
  }
}

Scene3DEntity* Scene3DDockWidget::entityFor(ObjectTopicId topic_id) const {
  auto it = entities_.find(topic_id.id);
  return it == entities_.end() ? nullptr : it->second.get();
}

void Scene3DDockWidget::setFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kAutoRoot);
  fixed_frame_mode_ = FixedFrameMode::kExplicit;
  if (mode_flipped) {
    emit fixedFrameModeChanged(false);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(frame);
}

void Scene3DDockWidget::setFixedFrameAutoRoot() {
  if (view_ == nullptr) {
    return;
  }
  const bool mode_flipped = (fixed_frame_mode_ == FixedFrameMode::kExplicit);
  fixed_frame_mode_ = FixedFrameMode::kAutoRoot;
  if (mode_flipped) {
    emit fixedFrameModeChanged(true);
    refreshFrameOverlayCombo();
  }
  applyResolvedFixedFrame(pickFixedFrame(available_frames_));
}

void Scene3DDockWidget::applyResolvedFixedFrame(const QString& frame) {
  if (frame.isEmpty() || view_ == nullptr) {
    return;
  }
  if (currentFixedFrame() == frame) {
    return;
  }
  view_->setFixedFrame(frame.toStdString());
  // Broadcast to every entity so their X/Y/Z colormap caches mark
  // themselves dirty (the next render refits).
  for (auto& [key, entity] : entities_) {
    if (entity != nullptr) {
      entity->setFixedFrame(frame);
    }
  }
  view_->update();
  emit currentFixedFrameChanged(frame);
  refreshFrameOverlayCombo();
  recomputeOrphanStates();
}

void Scene3DDockWidget::refreshFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  QSignalBlocker block(frame_overlay_combo_);
  frame_overlay_combo_->clear();
  for (const auto& row : available_frames_) {
    const QString name = QString::fromStdString(row.name);
    const QString display = QString(row.depth * 2, QChar(' ')) + name;
    frame_overlay_combo_->addItem(display, name);
  }
  // AutoRoot still drives the initial bootstrap pick — we just don't surface
  // a synthetic row for it. The user sees whichever named frame was
  // auto-resolved selected, and any subsequent pick flips to Explicit mode.
  const QString current = currentFixedFrame();
  const int idx = frame_overlay_combo_->findData(current);
  if (idx >= 0) {
    frame_overlay_combo_->setCurrentIndex(idx);
  }
  layoutFrameOverlayCombo();
}

void Scene3DDockWidget::onOverlayFramePicked(int /*index*/) {
  if (frame_overlay_combo_ == nullptr) {
    return;
  }
  const QString name = frame_overlay_combo_->currentData().toString();
  if (!name.isEmpty()) {
    setFixedFrame(name);
  }
}

void Scene3DDockWidget::layoutFrameOverlayCombo() {
  if (frame_overlay_combo_ == nullptr || view_ == nullptr) {
    return;
  }
  constexpr int kMargin = 8;
  // Combo width tracks the selected item only (so the overlay stays compact even
  // when one frame name is very long). The chrome (frame, padding, dropdown
  // arrow) is added by the style rather than a hardcoded slack — a fixed slack
  // underestimates the themed PJ::ComboBox and clips the text (e.g. "map" → "m").
  const QFontMetrics fm(frame_overlay_combo_->font());
  const QString current_text = frame_overlay_combo_->currentText();
  const auto combo_width_for = [&](const QString& text) {
    QStyleOptionComboBox opt;
    opt.initFrom(frame_overlay_combo_);
    const QSize content(fm.horizontalAdvance(text), fm.height());
    return frame_overlay_combo_->style()
        ->sizeFromContents(QStyle::CT_ComboBox, &opt, content, frame_overlay_combo_)
        .width();
  };
  const int natural_w = combo_width_for(current_text);
  // Cap to the dock width only once the dock has a real width — during
  // construction width() is 0 and the cap would collapse the combo.
  const int avail = width() - 2 * kMargin;
  const int w = (avail > 0) ? std::min(natural_w, avail) : natural_w;
  const int h = frame_overlay_combo_->sizeHint().height();
  const QPoint view_origin = view_->pos();
  frame_overlay_combo_->setGeometry(view_origin.x() + kMargin, view_origin.y() + kMargin, w, h);

  // Popup list keeps its own width so every row is fully readable when
  // expanded, even when the closed combo is narrow.
  if (auto* view = frame_overlay_combo_->view()) {
    int popup_w = natural_w;
    for (int i = 0; i < frame_overlay_combo_->count(); ++i) {
      popup_w = std::max(popup_w, combo_width_for(frame_overlay_combo_->itemText(i)));
    }
    view->setMinimumWidth(popup_w);
  }
  frame_overlay_combo_->raise();
}

void Scene3DDockWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  layoutFrameOverlayCombo();
}

Scene3DDockWidget::OrphanSnapshot Scene3DDockWidget::orphanState(ObjectTopicId topic_id) const {
  auto it = orphan_states_.find(topic_id.id);
  if (it == orphan_states_.end()) {
    return {};
  }
  return {it->second.is_orphan, it->second.reason};
}

void Scene3DDockWidget::setTopicVisible(ObjectTopicId topic_id, bool visible) {
  if (auto* entity = entityFor(topic_id)) {
    entity->setVisible(visible);
  }
}

void Scene3DDockWidget::reorderEntities(const std::vector<ObjectTopicId>& ordered_topic_ids) {
  if (view_ == nullptr) {
    return;
  }
  std::vector<pj::scene3d::Scene3DEntity*> ordered;
  ordered.reserve(ordered_topic_ids.size());
  for (const ObjectTopicId topic_id : ordered_topic_ids) {
    auto it = entities_.find(topic_id.id);
    if (it != entities_.end() && it->second != nullptr) {
      ordered.push_back(it->second.get());
    }
  }
  // The view applies it only if `ordered` is a full permutation of its
  // registry, then repaints. Saved state follows from view_->entities().
  view_->reorderEntities(ordered);
}

void Scene3DDockWidget::recomputeOrphanStates() {
  if (tf_buffer_ == nullptr) {
    return;
  }
  const QString fixed = currentFixedFrame();
  const std::string fixed_std = fixed.toStdString();

  // Hoist the all-frames snapshot once per recompute — orphan-ness is a
  // pure topology check at this point, so we don't need to ask the TF
  // buffer per entity. Connectivity uses latestCommonTime() which (per
  // tf_buffer.cpp) returns nullopt iff there is no common ancestor; it
  // does NOT require a TF sample at any particular timestamp, so this is
  // robust to dynamic edges whose first sample is in the future.
  std::unordered_set<std::string> known_frames;
  for (auto&& f : tf_buffer_->getAllFrames()) {
    known_frames.insert(std::move(f));
  }

  for (const auto& [key, entity_ptr] : entities_) {
    if (entity_ptr == nullptr) {
      continue;
    }
    const QString src = entity_ptr->sourceFrame();
    bool is_orphan = false;
    QString reason;
    if (src.isEmpty() || fixed.isEmpty()) {
      // Pending data — don't flag as orphan yet.
    } else if (src == fixed) {
      // Identity transform always succeeds; never orphan.
    } else {
      const std::string src_std = src.toStdString();
      // No-TF / disjoint-subtree case: in an MCAP that publishes N
      // pointclouds but no /tf or /tf_static, known_frames is empty;
      // each entity's source frame surfaces as an orphan root in the
      // overlay combo, and switching between them renders one
      // pointcloud at a time while the others get this "can't be
      // resolved" tooltip — accurate (no TF available) and intentional.
      if (known_frames.count(src_std) == 0) {
        is_orphan = true;
        reason = tr("Frame '%1' can't be resolved").arg(src);
      } else if (!tf_buffer_->latestCommonTime(fixed_std, src_std).has_value()) {
        is_orphan = true;
        reason = tr("Frame '%1' is not connected to fixed frame '%2'").arg(src, fixed);
      }
    }
    auto& state = orphan_states_[key];
    if (state.is_orphan != is_orphan || state.reason != reason) {
      state.is_orphan = is_orphan;
      state.reason = reason;
      ObjectTopicId tid;
      tid.id = static_cast<uint32_t>(key);
      emit entityOrphanChanged(tid, is_orphan, reason);
    }
  }
}

QDomElement Scene3DDockWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement root = doc.createElement(QStringLiteral("scene3d"));
  root.setAttribute(QStringLiteral("version"), QStringLiteral("1"));
  root.setAttribute(
      QStringLiteral("fixed_frame_mode"),
      fixed_frame_mode_ == FixedFrameMode::kAutoRoot ? QStringLiteral("auto_root") : QStringLiteral("explicit"));
  root.setAttribute(QStringLiteral("fixed_frame"), currentFixedFrame());

  QDomElement entities_el = doc.createElement(QStringLiteral("entities"));
  if (session_ != nullptr && view_ != nullptr) {
    // Persist in render order (view_->entities()), not entities_ map order, so
    // the user's drag-reordered draw order round-trips: xmlLoadState recreates
    // entities in document order.
    for (pj::scene3d::Scene3DEntity* entity_ptr : view_->entities()) {
      if (entity_ptr == nullptr) {
        continue;
      }
      const auto info = entity_ptr->info();
      const auto& desc = session_->objectStore().descriptor(info.topic_id);
      QDomElement entity_el = doc.createElement(QStringLiteral("entity"));
      entity_el.setAttribute(QStringLiteral("kind"), info.family_name.toLower());
      entity_el.setAttribute(QStringLiteral("dataset_id"), QString::number(desc.dataset_id));
      entity_el.setAttribute(QStringLiteral("topic_name"), QString::fromStdString(desc.topic_name));
      entity_el.setAttribute(QStringLiteral("object_type"), QString::fromUtf8(sdk::name(info.object_type).data()));
      entity_el.setAttribute(QStringLiteral("display_name"), info.display_name);
      entity_el.setAttribute(
          QStringLiteral("visible"), info.visible ? QStringLiteral("true") : QStringLiteral("false"));
      QDomElement payload = entity_ptr->xmlSaveState(doc);
      if (!payload.isNull()) {
        entity_el.appendChild(payload);
      }
      entities_el.appendChild(entity_el);
    }
  }
  root.appendChild(entities_el);
  return root;
}

bool Scene3DDockWidget::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("scene3d")) {
    return false;
  }
  // Defer applying fixed-frame state until after entities are attached so the
  // dock's fallback-frames cache is populated before any auto-resolve runs.
  const QString saved_mode = element.attribute(QStringLiteral("fixed_frame_mode"), QStringLiteral("auto_root"));
  const QString saved_frame = element.attribute(QStringLiteral("fixed_frame"));

  QDomElement entities_el = element.firstChildElement(QStringLiteral("entities"));
  if (session_ != nullptr) {
    for (QDomElement entity_el = entities_el.firstChildElement(QStringLiteral("entity")); !entity_el.isNull();
         entity_el = entity_el.nextSiblingElement(QStringLiteral("entity"))) {
      const auto dataset_id = static_cast<PJ::DatasetId>(entity_el.attribute(QStringLiteral("dataset_id")).toUInt());
      const QString topic_name = entity_el.attribute(QStringLiteral("topic_name"));
      const QString object_type_str = entity_el.attribute(QStringLiteral("object_type"));
      const QString display_name = entity_el.attribute(QStringLiteral("display_name"));
      const bool visible =
          entity_el.attribute(QStringLiteral("visible"), QStringLiteral("true")) == QStringLiteral("true");

      auto object_type_opt = sdk::parseBuiltinObjectType(object_type_str.toStdString());
      if (!object_type_opt.has_value()) {
        qCWarning(lcScene3DDock) << "xmlLoadState: unknown object_type" << object_type_str << ", skipping entity";
        continue;
      }
      auto topic_id_opt = session_->objectStore().findTopic(dataset_id, topic_name.toStdString());
      if (!topic_id_opt.has_value()) {
        qCInfo(lcScene3DDock) << "xmlLoadState: topic" << topic_name << "not present in current dataset, skipping";
        continue;
      }
      if (!addTopic(*topic_id_opt, *object_type_opt, display_name)) {
        continue;
      }
      if (!visible) {
        setTopicVisible(*topic_id_opt, false);
      }
      if (auto* entity = entityFor(*topic_id_opt); entity != nullptr) {
        QDomElement payload = entity_el.firstChildElement();
        if (!payload.isNull()) {
          entity->xmlLoadState(payload);
        }
      }
    }
  }

  if (saved_mode == QStringLiteral("explicit") && !saved_frame.isEmpty()) {
    setFixedFrame(saved_frame);
  } else {
    setFixedFrameAutoRoot();
  }
  return true;
}

}  // namespace PJ
