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
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/parse_locked.h"
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

std::pair<int64_t, int64_t> OccupancyGridLayer::timeRangeNs() const {
  return {ts_first_, ts_last_};
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
  parser_ = scene3d_ctx.session->parserForObjectTopic(topic_id_);
  parser_mutex_ = scene3d_ctx.session->parserMutexForObjectTopic(topic_id_);
  if (parser_ == nullptr) {
    qCWarning(lcOccGrid) << "attach: no parser for occupancy-grid topic" << topic_id_.id;
    return false;
  }

  // Discover the paired "<base>_updates" sibling in the same dataset (RViz
  // convention). Absent → base-only mode (each full grid is a keyframe).
  PJ::ObjectStore& store = scene3d_ctx.session->objectStore();
  const auto& desc = store.descriptor(topic_id_);
  const auto updates_id = store.findTopic(desc.dataset_id, desc.topic_name + "_updates");
  if (updates_id.has_value()) {
    updates_topic_ = *updates_id;
    updates_parser_ = scene3d_ctx.session->parserForObjectTopic(*updates_id);
    updates_parser_mutex_ = scene3d_ctx.session->parserMutexForObjectTopic(*updates_id);
  }

  grid_pass_.setColorScheme(color_scheme_);
  grid_pass_.setOpacity(opacity_);
  return bootstrap();
}

void OccupancyGridLayer::detach() {
  grid_pass_.clearGrid();
  parser_ = nullptr;
  parser_mutex_.reset();
  updates_parser_ = nullptr;
  updates_parser_mutex_.reset();
  updates_topic_.reset();
}

bool OccupancyGridLayer::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  auto obj = parseLocked(parser_, parser_mutex_, first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcOccGrid) << "bootstrap: parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* grid = std::any_cast<PJ::sdk::OccupancyGrid>(&obj->object);
  if (grid == nullptr) {
    return false;
  }
  source_frame_ = grid->frame_id;

  const auto base_range = store.timeRange(topic_id_);
  ts_first_ = base_range.first;
  ts_last_ = base_range.second;
  if (updates_topic_.has_value()) {
    const auto update_range = store.timeRange(*updates_topic_);
    ts_last_ = std::max(ts_last_, update_range.second);
  }

  if (!source_frame_.empty()) {
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

void OccupancyGridLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr || parser_ == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();

  // base_at(t): the latest full grid with ts <= t, decoded to sdk::OccupancyGrid.
  auto base_at = [this, &store](PJ::Timestamp t) -> std::optional<PJ::sdk::OccupancyGrid> {
    auto entry = store.latestAt(topic_id_, t);
    if (!entry.has_value() || entry->payload.bytes.empty()) {
      return std::nullopt;
    }
    auto obj = parseLocked(parser_, parser_mutex_, entry->timestamp, entry->payload);
    if (!obj.has_value()) {
      return std::nullopt;
    }
    const auto* grid = std::any_cast<PJ::sdk::OccupancyGrid>(&obj->object);
    if (grid == nullptr) {
      return std::nullopt;
    }
    return *grid;  // copy carries the anchor → bytes stay alive past the call
  };

  // updates_in(lo, hi): updates with lo < ts <= hi, ascending.
  auto updates_in = [this, &store](PJ::Timestamp lo, PJ::Timestamp hi) -> std::vector<PJ::sdk::OccupancyGridUpdate> {
    std::vector<PJ::sdk::OccupancyGridUpdate> out;
    if (!updates_topic_.has_value() || updates_parser_ == nullptr) {
      return out;
    }
    const PJ::ObjectTopicId id = *updates_topic_;
    const auto hi_idx = store.indexAt(id, hi);  // latest entry with ts <= hi
    if (!hi_idx.has_value()) {
      return out;  // nothing at or before hi
    }
    const auto lo_idx = store.indexAt(id, lo);  // latest with ts <= lo (excluded)
    const std::size_t start = lo_idx.has_value() ? *lo_idx + 1 : 0;
    const std::size_t count = store.entryCount(id);
    for (std::size_t i = start; i <= *hi_idx && i < count; ++i) {
      auto entry = store.at(id, i);
      if (!entry.has_value() || entry->payload.bytes.empty()) {
        continue;
      }
      auto obj = parseLocked(updates_parser_, updates_parser_mutex_, entry->timestamp, entry->payload);
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
  // the whole texture; an incremental update uploads just the changed rects.
  grid_pass_.setGrid(
      grid, update.kind != GridUpdate::Kind::Incremental,
      std::vector<CellRect>(update.dirty.begin(), update.dirty.end()));
}

void OccupancyGridLayer::setFixedFrame(const QString& frame) {
  // The grid is frame-relative; the render pass places it per-frame via its
  // FrameContext lookup against the fixed frame, so no re-decode is needed —
  // just request a paint.
  fixed_frame_ = frame;
  emit repaintRequested();
}

void OccupancyGridLayer::setTrackerTime(std::chrono::nanoseconds time) {
  tracker_time_ = time;
  renderAt(time.count());
  emit repaintRequested();
}

void OccupancyGridLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  grid_pass_.setVisible(visible);
  emit visibilityChanged(visible);
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
  grid_pass_.render(view_params, frame_ctx);
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

  auto* scheme_combo = new QComboBox(container);
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
