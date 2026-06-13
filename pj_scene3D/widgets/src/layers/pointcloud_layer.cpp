// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/pointcloud_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLoggingCategory>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/compressed_point_cloud.hpp"
#include "pj_base/builtin/point_cloud.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"                // PJ::fromRaw, PJ::toRaw
#include "pj_scene3d_core/camera/camera.h"  // AABB, expandAABB
#include "pj_scene3d_core/pointcloud.h"
#include "pj_scene3d_core/pointcloud_codecs.h"
#include "pj_scene3d_core/pointcloud_convert.h"  // convertCanonical, ConvertedPointCloud
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/DoubleScrubber.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcPointCloudLayer, "pj.scene3d.layer.pointcloud")

using PJ::sdk::BuiltinObjectType;
using PJ::sdk::CompressedPointCloud;
using PJ::sdk::PayloadView;
using PJ::sdk::PointCloud;

std::pair<float, float> computeScalarRange(const std::vector<float>& scalar) {
  float lo = std::numeric_limits<float>::max();
  float hi = std::numeric_limits<float>::lowest();
  for (float v : scalar) {
    if (!std::isfinite(v)) {
      continue;
    }
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  if (lo > hi) {
    return {0.0f, 1.0f};
  }
  if (hi - lo < 1e-6f) {
    hi = lo + 1.0f;
  }
  return {lo, hi};
}

QString defaultColorField(const QStringList& available) {
  if (available.isEmpty()) {
    return QString();
  }
  return available.contains(QStringLiteral("intensity")) ? QStringLiteral("intensity") : available.first();
}

// Square swatch button — same shape as CurveEditor's curve-color button so
// the pointcloud Solid picker reads as part of the same visual family.
class SolidColorSwatch : public QPushButton {
 public:
  explicit SolidColorSwatch(QColor color, QWidget* parent = nullptr) : QPushButton(parent), color_(color) {
    setCursor(Qt::PointingHandCursor);
    setFlat(true);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(22, 22);
  }
  void setColor(QColor color) {
    if (color_ == color) {
      return;
    }
    color_ = color;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color_);
    const int extent = std::max(6, std::min(width(), height()) - 4);
    const QRectF swatch_rect((width() - extent) / 2.0, (height() - extent) / 2.0, extent, extent);
    const qreal radius = std::max<qreal>(2.0, extent * 0.22);
    painter.drawRoundedRect(swatch_rect, radius, radius);
  }

 private:
  QColor color_;
};

}  // namespace

PointCloudLayer::PointCloudLayer(
    PJ::ObjectTopicId topic_id, QString display_name, BuiltinObjectType object_type, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)), object_type_(object_type) {
  // Push layer defaults to the pass at construction so the first paint
  // already reflects them (layer defaults are the design-spec values, not
  // the pass's "minimum visual change" defaults).
  cloud_pass_.setShape(shape_);
  cloud_pass_.setSizeMeters(size_meters_);
  cloud_pass_.setSizePixels(size_pixels_);
  cloud_pass_.setColorType(color_type_);
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  cloud_pass_.setColormap(colormap_);
  cloud_pass_.setInvertLut(invert_lut_);
}

PointCloudLayer::~PointCloudLayer() = default;

PJ::SceneLayerInfo PointCloudLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = object_type_,
      .display_name = display_name_,
      .family_name = QStringLiteral("PointCloud"),
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> PointCloudLayer::timeRange() const {
  return PJ::liveTopicTimeRange(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_);
}

QStringList PointCloudLayer::fallbackFrames() const {
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  return out;
}

QString PointCloudLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement PointCloudLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(QStringLiteral("pointcloud"));
  auto shape_str = [&]() -> QString {
    switch (shape_) {
      case PointcloudRenderPass::Shape::kSphere:
        return QStringLiteral("sphere");
      case PointcloudRenderPass::Shape::kPoint:
        return QStringLiteral("point");
      case PointcloudRenderPass::Shape::kCube:
        return QStringLiteral("cube");
    }
    return QStringLiteral("sphere");
  }();
  auto colormap_str = [&]() -> QString {
    switch (colormap_) {
      case PointcloudRenderPass::Colormap::kTurbo:
        return QStringLiteral("turbo");
      case PointcloudRenderPass::Colormap::kViridis:
        return QStringLiteral("viridis");
      case PointcloudRenderPass::Colormap::kPlasma:
        return QStringLiteral("plasma");
      case PointcloudRenderPass::Colormap::kGrayscale:
        return QStringLiteral("grayscale");
    }
    return QStringLiteral("turbo");
  }();
  el.setAttribute(QStringLiteral("shape"), shape_str);
  el.setAttribute(QStringLiteral("size_meters"), QString::number(static_cast<double>(size_meters_), 'g', 6));
  el.setAttribute(QStringLiteral("size_pixels"), QString::number(size_pixels_));
  el.setAttribute(
      QStringLiteral("color_type"),
      color_type_ == PointcloudRenderPass::ColorType::kSolid ? QStringLiteral("solid") : QStringLiteral("field"));
  el.setAttribute(QStringLiteral("color_field"), QString::fromStdString(color_field_));
  el.setAttribute(QStringLiteral("solid_color"), solid_color_.name(QColor::HexRgb));
  el.setAttribute(QStringLiteral("colormap"), colormap_str);
  el.setAttribute(QStringLiteral("auto_range"), auto_range_ ? QStringLiteral("true") : QStringLiteral("false"));
  el.setAttribute(QStringLiteral("invert_lut"), invert_lut_ ? QStringLiteral("true") : QStringLiteral("false"));
  el.setAttribute(QStringLiteral("range_min"), QString::number(static_cast<double>(manual_range_min_), 'g', 6));
  el.setAttribute(QStringLiteral("range_max"), QString::number(static_cast<double>(manual_range_max_), 'g', 6));
  return el;
}

bool PointCloudLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("pointcloud")) {
    return false;
  }
  const QString shape_str = element.attribute(QStringLiteral("shape"), QStringLiteral("sphere"));
  if (shape_str == QStringLiteral("point")) {
    setShape(PointcloudRenderPass::Shape::kPoint);
  } else if (shape_str == QStringLiteral("cube")) {
    setShape(PointcloudRenderPass::Shape::kCube);
  } else {
    setShape(PointcloudRenderPass::Shape::kSphere);
  }

  bool ok = false;
  const float size_m = element.attribute(QStringLiteral("size_meters"), QStringLiteral("0.01")).toFloat(&ok);
  if (ok) {
    setSizeMeters(size_m);
  }
  const int size_px = element.attribute(QStringLiteral("size_pixels"), QStringLiteral("2")).toInt(&ok);
  if (ok) {
    setSizePixels(size_px);
  }

  const QString color_type_str = element.attribute(QStringLiteral("color_type"), QStringLiteral("field"));
  setColorType(
      color_type_str == QStringLiteral("solid") ? PointcloudRenderPass::ColorType::kSolid
                                                : PointcloudRenderPass::ColorType::kField);
  if (element.hasAttribute(QStringLiteral("color_field"))) {
    setColorField(element.attribute(QStringLiteral("color_field")));
  }
  if (element.hasAttribute(QStringLiteral("solid_color"))) {
    const QColor c(element.attribute(QStringLiteral("solid_color")));
    if (c.isValid()) {
      setSolidColor(c);
    }
  }

  const QString cm_str = element.attribute(QStringLiteral("colormap"), QStringLiteral("turbo"));
  if (cm_str == QStringLiteral("viridis")) {
    setColormap(PointcloudRenderPass::Colormap::kViridis);
  } else if (cm_str == QStringLiteral("plasma")) {
    setColormap(PointcloudRenderPass::Colormap::kPlasma);
  } else if (cm_str == QStringLiteral("grayscale")) {
    setColormap(PointcloudRenderPass::Colormap::kGrayscale);
  } else {
    setColormap(PointcloudRenderPass::Colormap::kTurbo);
  }

  setInvertLut(element.attribute(QStringLiteral("invert_lut")) == QStringLiteral("true"));

  bool ok_min = false;
  bool ok_max = false;
  const float r_min = element.attribute(QStringLiteral("range_min"), QStringLiteral("0")).toFloat(&ok_min);
  const float r_max = element.attribute(QStringLiteral("range_max"), QStringLiteral("1")).toFloat(&ok_max);
  if (ok_min && ok_max) {
    setManualRange(r_min, r_max);
  }
  // Auto-range is applied last so the manual range above doesn't get
  // overwritten by an auto-recompute on the next cloud swap.
  setAutoRange(element.attribute(QStringLiteral("auto_range"), QStringLiteral("true")) == QStringLiteral("true"));

  return true;
}

bool PointCloudLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcPointCloudLayer) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcPointCloudLayer) << "attach: no parser for topic_id=" << topic_id_.id;
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  ts_first_.reset();  // re-derive: a detachless re-attach (reload path) may see an emptied store
  if (store.entryCount(topic_id_) > 0) {
    ts_first_ = store.timeRange(topic_id_).first;
  }
  // A reload swaps this topic's bytes under a stable ObjectTopicId, so every
  // cached sample identity must reset with them (see onDatasetAboutToBeReplaced).
  // disconnect first: the reload path re-attaches without an intervening detach.
  disconnect(reload_connection_);
  reload_connection_ = connect(
      ctx_.session, &PJ::SessionManager::datasetAboutToBeReplaced, this, &PointCloudLayer::onDatasetAboutToBeReplaced);
  if (!bootstrap()) {
    qCWarning(lcPointCloudLayer) << "attach: bootstrap failed for topic_id=" << topic_id_.id;
    // Continue anyway — render will silently skip until a sample arrives.
  }
  if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
  return true;
}

void PointCloudLayer::detach() {
  disconnect(reload_connection_);
  reload_connection_ = {};
  drop_inflight_result_ = false;
  ts_first_.reset();
  decoded_at_ns_.reset();
  if (decode_watcher_ != nullptr) {
    decode_watcher_->disconnect(this);  // a late result must not touch a detached layer
    decode_watcher_->cancel();
    // Drop the watcher entirely: ensureDecodeWorker() only wires the finished signal
    // when it creates one, so a kept-but-disconnected watcher would leave a
    // re-attached layer decoding into the void.
    decode_watcher_->deleteLater();
    decode_watcher_ = nullptr;
  }
  pending_.reset();
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  inflight_ = {};
  wanted_ = {};
  failed_id_ = {};
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  ctx_ = {};
  cloud_pass_.setActiveCloud(nullptr);
  world_bounds_.reset();
}

void PointCloudLayer::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  // Scalars are source-frame payload bytes (read in convertCanonical); the fixed-frame
  // TF is applied only in the vertex shader. Switching the fixed frame therefore cannot
  // change any scalar or its auto-range — only the rendered transform — so just repaint
  // so the shader re-runs with the new source->fixed transform; no re-decode/refit needed.
  emit repaintRequested();
}

void PointCloudLayer::setTrackerTime(PJ::Timepoint time) {
  decoded_at_ns_ = time;
  // Defer the heavy decode (parse + convertCanonical + GPU upload) to render():
  // a fast scrub fires many ticks but Qt coalesces the repaints into one paint,
  // so only the final cloud is decoded instead of every skipped frame x N topics.
  tracker_dirty_ = true;
  if (visible_) {
    emit repaintRequested();
  }
}

void PointCloudLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  cloud_pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Catch-up on un-hide is the dock's job: SceneDockWidget::setLayerVisible
  // re-delivers the last tracker time (hidden layers receive no ticks), which
  // marks tracker_dirty_ so the next paint decodes at the playhead.
  emit repaintRequested();
}

void PointCloudLayer::initializeGL() {
  cloud_pass_.initializeGL();
}

void PointCloudLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  // Drain a pending tracker move here (coalesced to one decode per painted frame).
  // renderAt's own SampleId guard makes this cheap when the active sample is
  // unchanged. frame_ctx.time and decoded_at_ns_ track the same playhead (the dock
  // paints at the tracker time); refreshNow() re-decodes from the latter on
  // color-field / range changes.
  if (visible_ && tracker_dirty_) {
    tracker_dirty_ = false;
    renderAt(PJ::toRaw(frame_ctx.time));
  }
  cloud_pass_.render(view_params, frame_ctx);
}

void PointCloudLayer::releaseGL() {
  cloud_pass_.releaseGL();
}

QWidget* PointCloudLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(6);

  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(6);
  outer->addLayout(form);

  // --- Shape ----------------------------------------------------------------
  auto* shape_combo = new QComboBox(container);
  shape_combo->addItem(tr("sphere"), static_cast<int>(PointcloudRenderPass::Shape::kSphere));
  shape_combo->addItem(tr("point"), static_cast<int>(PointcloudRenderPass::Shape::kPoint));
  shape_combo->addItem(tr("cube"), static_cast<int>(PointcloudRenderPass::Shape::kCube));
  shape_combo->setCurrentIndex(static_cast<int>(shape_));
  form->addRow(tr("Shape:"), shape_combo);

  // --- Point size (units depend on shape) ----------------------------------
  auto* size_spin = new PJ::DoubleScrubber(container);
  const auto apply_size_units = [size_spin, this]() {
    QSignalBlocker block(size_spin);
    if (shape_ == PointcloudRenderPass::Shape::kPoint) {
      size_spin->setSuffix(QStringLiteral(" px"));
      size_spin->setDecimals(1);
      size_spin->setSingleStep(0.5);
      size_spin->setRange(1.0, 32.0);
      size_spin->setValue(static_cast<double>(size_pixels_));
    } else {
      size_spin->setSuffix(QStringLiteral(" m"));
      size_spin->setDecimals(3);
      size_spin->setSingleStep(0.001);
      size_spin->setRange(0.001, 10.0);
      size_spin->setValue(static_cast<double>(size_meters_));
    }
  };
  apply_size_units();
  form->addRow(tr("Point size:"), size_spin);

  // --- Color type (Solid + per-field) ---------------------------------------
  auto* color_type_combo = new QComboBox(container);
  const auto rebuild_color_type_combo = [color_type_combo, this]() {
    QSignalBlocker block(color_type_combo);
    color_type_combo->clear();
    color_type_combo->addItem(tr("Solid"), QString{});
    for (const QString& f : available_color_fields_) {
      color_type_combo->addItem(tr("Field: %1").arg(f), f);
    }
    if (color_type_ == PointcloudRenderPass::ColorType::kSolid) {
      color_type_combo->setCurrentIndex(0);
    } else {
      const int idx = color_type_combo->findData(QString::fromStdString(color_field_));
      color_type_combo->setCurrentIndex(idx >= 0 ? idx : 0);
    }
  };
  rebuild_color_type_combo();
  form->addRow(tr("Color type:"), color_type_combo);

  // --- Color config swap section --------------------------------------------
  auto* color_stack = new QStackedWidget(container);
  form->addRow(tr("Color config:"), color_stack);

  // Page 0: Solid — rounded-square swatch, click opens ColorPickerPopup
  // (same recipe as CurveEditor for visual consistency across panels).
  auto* solid_page = new QWidget(color_stack);
  auto* solid_layout = new QHBoxLayout(solid_page);
  solid_layout->setContentsMargins(0, 0, 0, 0);
  solid_layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  auto* color_button = new SolidColorSwatch(solid_color_, solid_page);
  solid_layout->addWidget(color_button);
  solid_layout->addStretch();
  color_stack->addWidget(solid_page);

  // Page 1: Gradient.
  auto* gradient_page = new QWidget(color_stack);
  auto* gradient_layout = new QFormLayout(gradient_page);
  gradient_layout->setContentsMargins(0, 0, 0, 0);
  gradient_layout->setSpacing(6);

  // Explicit toggle-button style so checkable QPushButtons read as buttons
  // (not labels) regardless of the underlying Qt platform style. 4 px corner
  // radius mirrors pj_widgets DoubleScrubber — the panel's reference style.
  static constexpr auto kToggleButtonQss =
      "QPushButton { border: 1px solid #8c8c8c; border-radius: 4px; padding: 2px 10px; background-color: "
      "palette(button); }"
      "QPushButton:hover { border-color: #5b8fd9; }"
      "QPushButton:checked { background-color: #b7d2f5; border-color: #5b8fd9; }";

  // --- Colormap row: combo + invert toggle right-aligned ---
  auto* colormap_row = new QWidget(gradient_page);
  auto* colormap_row_layout = new QHBoxLayout(colormap_row);
  colormap_row_layout->setContentsMargins(0, 0, 0, 0);
  colormap_row_layout->setSpacing(6);
  auto* colormap_combo = new QComboBox(colormap_row);
  colormap_combo->addItem(QStringLiteral("turbo"), static_cast<int>(PointcloudRenderPass::Colormap::kTurbo));
  colormap_combo->addItem(QStringLiteral("viridis"), static_cast<int>(PointcloudRenderPass::Colormap::kViridis));
  colormap_combo->addItem(QStringLiteral("plasma"), static_cast<int>(PointcloudRenderPass::Colormap::kPlasma));
  colormap_combo->addItem(QStringLiteral("grayscale"), static_cast<int>(PointcloudRenderPass::Colormap::kGrayscale));
  colormap_combo->setCurrentIndex(static_cast<int>(colormap_));
  colormap_row_layout->addWidget(colormap_combo, 1);
  auto* invert_btn = new QPushButton(colormap_row);
  invert_btn->setCheckable(true);
  invert_btn->setChecked(invert_lut_);
  invert_btn->setFocusPolicy(Qt::NoFocus);
  invert_btn->setStyleSheet(kToggleButtonQss);
  invert_btn->setIcon(QIcon(QStringLiteral(":/resources/svg/invert.svg")));
  invert_btn->setIconSize(QSize(20, 20));
  // Match the standard icon-button extent used across the app
  // (icon_size + icon_padding = 24 — see CurveListPanel / TimelineWidget).
  invert_btn->setFixedSize(24, 24);
  invert_btn->setToolTip(tr("Invert colormap"));
  colormap_row_layout->addWidget(invert_btn, 0);
  // Label-less — the combo carries the colormap name itself.
  gradient_layout->addRow(colormap_row);

  color_stack->addWidget(gradient_page);

  // Initial stack page.
  color_stack->setCurrentIndex(color_type_ == PointcloudRenderPass::ColorType::kSolid ? 0 : 1);

  // --- Range: row on the OUTER form (auto button as the field) ---
  auto* auto_btn = new QPushButton(tr("auto"), container);
  auto_btn->setCheckable(true);
  auto_btn->setChecked(auto_range_);
  auto_btn->setFocusPolicy(Qt::NoFocus);
  auto_btn->setStyleSheet(kToggleButtonQss);
  auto* auto_row = new QWidget(container);
  auto* auto_row_layout = new QHBoxLayout(auto_row);
  auto_row_layout->setContentsMargins(0, 0, 0, 0);
  auto_row_layout->addWidget(auto_btn);
  auto_row_layout->addStretch();
  form->addRow(tr("Range:"), auto_row);

  // Range Min/Max also live on the OUTER form so their labels align with
  // Shape / Point size / Color type. Only visible in gradient mode with
  // auto-range OFF.
  auto* range_min_spin = new PJ::DoubleScrubber(container);
  range_min_spin->setDecimals(4);
  range_min_spin->setRange(-1e9, 1e9);
  range_min_spin->setValue(static_cast<double>(manual_range_min_));
  form->addRow(tr("Range Min:"), range_min_spin);

  auto* range_max_spin = new PJ::DoubleScrubber(container);
  range_max_spin->setDecimals(4);
  range_max_spin->setRange(-1e9, 1e9);
  range_max_spin->setValue(static_cast<double>(manual_range_max_));
  form->addRow(tr("Range Max:"), range_max_spin);

  // Range step is 0.1 for spatial fields (x/y/z, always in metres) and 1.0
  // otherwise (intensity, reflectance, ring index, …). Recomputed whenever
  // the color field changes.
  const auto apply_range_step = [this, range_min_spin, range_max_spin]() {
    const bool is_spatial = color_field_ == "x" || color_field_ == "y" || color_field_ == "z";
    const double step = is_spatial ? 0.1 : 1.0;
    range_min_spin->setSingleStep(step);
    range_max_spin->setSingleStep(step);
  };
  apply_range_step();

  // Visibility:
  //   Range:/auto row → shown in gradient mode; hidden in solid mode.
  //   Range Min/Max   → shown only in gradient AND auto-range OFF.
  const auto apply_range_visibility = [form, auto_row, range_min_spin, range_max_spin, this](bool auto_on) {
    const bool gradient = color_type_ == PointcloudRenderPass::ColorType::kField;
    const bool show_spins = !auto_on && gradient;
    auto_row->setVisible(gradient);
    if (auto* lbl = form->labelForField(auto_row)) {
      lbl->setVisible(gradient);
    }
    range_min_spin->setVisible(show_spins);
    range_max_spin->setVisible(show_spins);
    if (auto* lbl = form->labelForField(range_min_spin)) {
      lbl->setVisible(show_spins);
    }
    if (auto* lbl = form->labelForField(range_max_spin)) {
      lbl->setVisible(show_spins);
    }
  };
  apply_range_visibility(auto_range_);

  // ---- User → layer wires -------------------------------------------------

  QObject::connect(
      shape_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, shape_combo, apply_size_units](int) {
        const auto v = static_cast<PointcloudRenderPass::Shape>(shape_combo->currentData().toInt());
        setShape(v);
        apply_size_units();
      });

  QObject::connect(size_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) {
    if (shape_ == PointcloudRenderPass::Shape::kPoint) {
      setSizePixels(static_cast<int>(v));
    } else {
      setSizeMeters(static_cast<float>(v));
    }
  });

  QObject::connect(
      color_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this, color_type_combo, color_stack, apply_range_step, apply_range_visibility](int) {
        const QString data = color_type_combo->currentData().toString();
        if (data.isEmpty()) {
          setColorType(PointcloudRenderPass::ColorType::kSolid);
          color_stack->setCurrentIndex(0);
        } else {
          setColorType(PointcloudRenderPass::ColorType::kField);
          setColorField(data);
          color_stack->setCurrentIndex(1);
          apply_range_step();
        }
        apply_range_visibility(auto_range_);
      });

  QObject::connect(color_button, &QPushButton::clicked, this, [this, container, color_button]() {
    auto* popup = new PJ::ColorPickerPopup(container);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setColor(solid_color_);
    QObject::connect(popup, &PJ::ColorPickerPopup::colorChanged, this, [this, color_button](QColor c) {
      if (c.isValid()) {
        setSolidColor(c);
        color_button->setColor(c);
      }
    });
    const QPoint global = color_button->mapToGlobal(QPoint(0, color_button->height() + 2));
    popup->move(global);
    popup->show();
  });

  QObject::connect(
      colormap_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, colormap_combo](int) {
        setColormap(static_cast<PointcloudRenderPass::Colormap>(colormap_combo->currentData().toInt()));
      });

  QObject::connect(auto_btn, &QPushButton::toggled, this, [this, apply_range_visibility](bool on) {
    setAutoRange(on);
    apply_range_visibility(on);
  });

  QObject::connect(invert_btn, &QPushButton::toggled, this, &PointCloudLayer::setInvertLut);

  const auto push_manual_range = [this, range_min_spin, range_max_spin]() {
    setManualRange(static_cast<float>(range_min_spin->value()), static_cast<float>(range_max_spin->value()));
  };
  QObject::connect(range_min_spin, &PJ::DoubleScrubber::valueChanged, this, push_manual_range);
  QObject::connect(range_max_spin, &PJ::DoubleScrubber::valueChanged, this, push_manual_range);

  // ---- Layer → widget wires (out-of-band changes / auto-range refresh) ----

  QPointer<QComboBox> safe_type(color_type_combo);
  QObject::connect(this, &PointCloudLayer::colorFieldsChanged, container, [safe_type, rebuild_color_type_combo]() {
    if (!safe_type) {
      return;
    }
    rebuild_color_type_combo();
  });

  QPointer<PJ::DoubleScrubber> safe_min(range_min_spin);
  QPointer<PJ::DoubleScrubber> safe_max(range_max_spin);
  QObject::connect(this, &PointCloudLayer::autoRangeComputed, container, [safe_min, safe_max](float lo, float hi) {
    if (safe_min) {
      QSignalBlocker block(safe_min.data());
      safe_min->setValue(static_cast<double>(lo));
    }
    if (safe_max) {
      QSignalBlocker block(safe_max.data());
      safe_max->setValue(static_cast<double>(hi));
    }
  });

  // Uniform row height keyed to the DoubleScrubber's natural sizeHint —
  // the scrubber is the reference style for the panel, so combos / spins
  // / toggles all clamp to its height. The scrubber itself is left alone.
  const int row_h = std::max(size_spin->sizeHint().height(), 22);
  for (QWidget* w :
       {static_cast<QWidget*>(shape_combo), static_cast<QWidget*>(color_type_combo),
        static_cast<QWidget*>(colormap_combo), static_cast<QWidget*>(auto_btn)}) {
    w->setMinimumHeight(row_h);
    w->setMaximumHeight(row_h);
  }
  // Invert button: square, sized to the row height so it lines up with
  // the colormap combo it sits beside.
  invert_btn->setFixedSize(row_h, row_h);
  invert_btn->setIconSize(QSize(row_h - 6, row_h - 6));

  return container;
}

void PointCloudLayer::setColorField(const QString& field) {
  const std::string new_field = field.toStdString();
  if (color_field_ == new_field) {
    return;
  }
  color_field_ = new_field;
  range_dirty_ = true;
  emit currentColorFieldChanged(field);
  refreshNow();
}

void PointCloudLayer::setShape(PointcloudRenderPass::Shape shape) {
  if (shape_ == shape) {
    return;
  }
  shape_ = shape;
  cloud_pass_.setShape(shape_);
  emit repaintRequested();
}

void PointCloudLayer::setSizeMeters(float meters) {
  if (size_meters_ == meters) {
    return;
  }
  size_meters_ = meters;
  cloud_pass_.setSizeMeters(size_meters_);
  emit repaintRequested();
}

void PointCloudLayer::setSizePixels(int pixels) {
  if (size_pixels_ == pixels) {
    return;
  }
  size_pixels_ = pixels;
  cloud_pass_.setSizePixels(size_pixels_);
  emit repaintRequested();
}

void PointCloudLayer::setColorType(PointcloudRenderPass::ColorType type) {
  if (color_type_ == type) {
    return;
  }
  color_type_ = type;
  cloud_pass_.setColorType(color_type_);
  emit repaintRequested();
}

void PointCloudLayer::setSolidColor(QColor color) {
  if (solid_color_ == color) {
    return;
  }
  solid_color_ = color;
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  emit repaintRequested();
}

void PointCloudLayer::setColormap(PointcloudRenderPass::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  cloud_pass_.setColormap(colormap_);
  emit repaintRequested();
}

void PointCloudLayer::setInvertLut(bool invert) {
  if (invert_lut_ == invert) {
    return;
  }
  invert_lut_ = invert;
  cloud_pass_.setInvertLut(invert_lut_);
  emit repaintRequested();
}

void PointCloudLayer::setAutoRange(bool enable) {
  if (auto_range_ == enable) {
    return;
  }
  auto_range_ = enable;
  if (auto_range_) {
    // Force the next cloud swap to recompute, which will also re-emit
    // autoRangeComputed so the panel's hidden spinboxes refresh.
    range_dirty_ = true;
    refreshNow();
  } else {
    // Pin the pass to whatever manual values the layer is currently
    // holding so the colormap doesn't snap to stale auto-computed bounds.
    cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
    emit repaintRequested();
  }
}

void PointCloudLayer::setManualRange(float min_value, float max_value) {
  if (manual_range_min_ == min_value && manual_range_max_ == max_value) {
    return;
  }
  manual_range_min_ = min_value;
  manual_range_max_ = max_value;
  if (!auto_range_) {
    cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
    emit repaintRequested();
  }
}

bool PointCloudLayer::bootstrap() {
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
    qCWarning(lcPointCloudLayer) << "bootstrap parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  // Compressed topic (Draco / Cloudini): the wrapper gives frame_id without a
  // decode, but the field list needs one. Decode the first sample asynchronously so
  // attach never blocks the UI — fields populate when the result lands.
  if (const auto* cpc = std::any_cast<CompressedPointCloud>(&obj->object)) {
    requestDecode(*cpc, SampleId{first->timestamp, first->payload.bytes.size()});
    return true;
  }

  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return false;
  }
  updateSourceFrame(sdk_cloud->frame_id);
  populateColorFields(*sdk_cloud);
  return true;
}

void PointCloudLayer::populateColorFields(const PointCloud& cloud) {
  available_color_fields_.clear();
  for (const auto& f : cloud.fields) {
    if (f.name == "timestamp") {
      continue;
    }
    available_color_fields_.append(QString::fromStdString(f.name));
  }
  // Preserve an already-chosen field (e.g. one restored by xmlLoadState before the
  // async first decode landed) when it's still present; only fall back to the default
  // when the current selection is empty or no longer valid.
  if (color_field_.empty() || !available_color_fields_.contains(QString::fromStdString(color_field_))) {
    color_field_ = defaultColorField(available_color_fields_).toStdString();
  }
  emit colorFieldsChanged(available_color_fields_);
  if (!color_field_.empty()) {
    emit currentColorFieldChanged(QString::fromStdString(color_field_));
  }
}

void PointCloudLayer::updateSourceFrame(const std::string& frame_id) {
  // frame_id can change across samples (e.g. a recording bridging a config reload);
  // keep the dock's orphan check and the fallback-frames list current.
  if (frame_id == source_frame_) {
    return;
  }
  source_frame_ = frame_id;
  emit sourceFrameChanged(QString::fromStdString(source_frame_));
  emit fallbackFramesChanged(fallbackFrames());
}

void PointCloudLayer::pushCloud(const PointCloud& cloud, SampleId id) {
  updateSourceFrame(cloud.frame_id);
  if (available_color_fields_.isEmpty() && !cloud.fields.empty()) {
    // Self-heal after a failed bootstrap (topic attached before its first sample):
    // the first cloud that reaches the GPU also reveals the field set.
    populateColorFields(cloud);
  }
  ConvertedPointCloud converted = convertCanonical(cloud, color_field_);
  DecodedPointCloud& decoded = converted.cloud;

  // convertCanonical accumulates the finite-point AABB in its single decode pass, so the
  // source-frame bounds come back for free (TF is applied per-render in the shader, so the
  // decoded positions stay in the cloud's own frame).
  world_bounds_ = converted.bounds.valid ? std::optional<AABB>{converted.bounds} : std::nullopt;

  // The scalar is read straight from the message bytes in the cloud's source frame; the
  // fixed-frame TF is applied only in the vertex shader, so changing the fixed frame cannot
  // change any scalar — even for x/y/z coloring. Recompute the auto-range only when it is
  // actually dirty (a new color field or a re-enabled auto-range), never per push.
  if (!decoded.scalar.empty() && auto_range_ && range_dirty_) {
    const auto [lo, hi] = computeScalarRange(decoded.scalar);
    cloud_pass_.setColormapRange(lo, hi);
    range_dirty_ = false;
    emit autoRangeComputed(lo, hi);
  }

  cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>(std::move(decoded)));
  last_pushed_id_ = id;
  last_pushed_color_field_ = color_field_;
  emit repaintRequested();
}

void PointCloudLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  const SampleId id{resolved->timestamp, resolved->payload.bytes.size()};
  // This sample is now the one the tracker wants — set it on EVERY path (also the
  // raw push and the early-skip below), so a stale in-flight compressed decode of
  // another sample is classified stale in onDecodeFinished instead of overwriting
  // a newer cloud on a mixed-mode topic.
  wanted_ = id;
  // The tracker ticks at ~60 Hz but a topic publishes far slower, so the common case
  // is "same sample, same color field" — skip the whole parse/convert/upload then.
  // range_dirty_ only matters when auto-range will actually recompute in pushCloud.
  if (id == last_pushed_id_ && color_field_ == last_pushed_color_field_ && !(auto_range_ && range_dirty_)) {
    return;
  }
  // Compressed sample already decoded? Re-convert from cache so repaints /
  // color-field changes don't re-run the codec (or even the wrapper parse).
  if (decoded_cache_ && decoded_cache_id_ == id) {
    pushCloud(*decoded_cache_, id);
    return;
  }
  if (id == failed_id_) {
    return;  // known-undecodable sample; the memo holds until a reload swaps the bytes
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  auto obj = parseLocked(binding, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcPointCloudLayer) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }

  // The mode is derived per-sample rather than latched at bootstrap, so a topic
  // whose first sample arrives only after attach (failed bootstrap) still renders.
  if (const auto* cpc = std::any_cast<CompressedPointCloud>(&obj->object)) {
    requestDecode(*cpc, id);  // off the UI thread; pushes when ready
    return;
  }

  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return;
  }
  pushCloud(*sdk_cloud, id);
}

void PointCloudLayer::ensureDecodeWorker() {
  if (decode_watcher_ == nullptr) {
    decode_watcher_ = new QFutureWatcher<DecodeResult>(this);
    connect(decode_watcher_, &QFutureWatcher<DecodeResult>::finished, this, &PointCloudLayer::onDecodeFinished);
  }
}

void PointCloudLayer::requestDecode(const CompressedPointCloud& cloud, SampleId id) {
  updateSourceFrame(cloud.frame_id);
  wanted_ = id;  // a late decode of any other sample must not be painted
  ensureDecodeWorker();
  if (inflight_ == id) {
    pending_.reset();  // the running decode is wanted again; drop any superseded sample
    return;
  }
  // Gate on inflight_, NOT QFutureWatcher::isRunning(): a future can be finished with
  // its finished() event still queued, and setFuture() in that window would silently
  // drop the event (and the decoded result with it). inflight_ is reset only once
  // onDecodeFinished() actually ran, so the pending queue catches that window too.
  if (inflight_ != SampleId{}) {
    pending_ = PendingDecode{cloud, id};  // the shared anchor keeps the blob alive
    return;
  }
  startDecode(cloud, id);
}

void PointCloudLayer::startDecode(const CompressedPointCloud& cloud, SampleId id) {
  inflight_ = id;
  ++start_decode_count_;
  // Capture the wrapper by value — its BufferAnchor keeps the compressed bytes alive on
  // the worker. This requires the anchored bytes to be IMMUTABLE, not merely alive:
  // every in-tree producer either deep-copies (canonical codec) or anchors a const
  // ObjectStore buffer, but a zero-copy parser reusing a scratch buffer would race the
  // worker. The task touches no layer state (decodeCompressedPointCloud is a pure core
  // function), so it stays safe even if the layer is destroyed mid-decode.
  CompressedPointCloud snapshot = cloud;
  decode_watcher_->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), id]() -> DecodeResult {
    DecodeResult result;
    result.id = id;
    // Exception barrier: decodeCompressedPointCloud is barriered internally, but
    // the residue here (shared_ptr control block, error-string copy) can still
    // throw under memory pressure. An exception stored in the future would be
    // rethrown by decode_watcher_->result() ON THE GUI THREAD and terminate the
    // app — module policy forbids worker exceptions reaching the GUI thread.
    try {
      auto decoded = decodeCompressedPointCloud(snapshot);
      if (decoded.has_value()) {
        result.cloud = std::make_shared<PointCloud>(std::move(decoded.value()));
      } else {
        result.error = QString::fromStdString(decoded.error());
      }
    } catch (const std::bad_alloc&) {
      result.cloud.reset();
      result.error = QStringLiteral("out of memory finalizing decoded point cloud");  // no-alloc literal
    } catch (const std::exception& ex) {
      result.cloud.reset();
      result.error = QString::fromUtf8(ex.what());
    } catch (...) {
      result.cloud.reset();
      result.error = QStringLiteral("unknown exception decoding point cloud");
    }
    return result;
  }));
}

void PointCloudLayer::onDecodeFinished() {
  const DecodeResult result = decode_watcher_->result();
  inflight_ = {};
  // A dataset reload while this sample decoded means the result holds pre-reload
  // content whose (timestamp, size) key may alias the new bytes: neither the
  // cache nor the failure memo may keep it (wanted_ was reset too, so it can
  // never paint). Still fall through to the pending drain below.
  const bool drop_result = drop_inflight_result_;
  drop_inflight_result_ = false;
  // Render this result only if it's still the sample the tracker wants. If the user
  // scrubbed away while it decoded — even back onto a cached frame — it's stale: cache
  // it (cheap, useful on scrub-back) but don't paint it over the live frame.
  const bool is_current = result.id == wanted_;

  if (drop_result) {
    // Discarded pre-reload result: no cache, no failed_id_, no paint.
  } else if (result.cloud) {
    decoded_cache_ = result.cloud;
    decoded_cache_id_ = result.id;
    if (available_color_fields_.isEmpty()) {
      populateColorFields(*result.cloud);  // first successful decode reveals the field set
    }
    if (is_current) {
      pushCloud(*result.cloud, result.id);
    }
  } else {
    failed_id_ = result.id;  // memoize: renderAt won't re-request a sample that can never decode
    if (is_current) {
      qCWarning(lcPointCloudLayer) << "compressed point cloud decode failed:" << result.error;
      // Match the raw path's malformed-cloud behavior (empty convertCanonical):
      // clear the view rather than leaving the previous sample's points painted
      // at the wrong tracker time.
      cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>());
      world_bounds_.reset();
      last_pushed_id_ = {};
      last_pushed_color_field_.clear();
      emit repaintRequested();
    }
  }

  // Drain the latest-wins queue — but only if the queued sample is still the one
  // the tracker wants; decoding a stale one would evict the wanted cache entry.
  if (pending_.has_value()) {
    const PendingDecode next = std::move(*pending_);
    pending_.reset();
    if (next.id == wanted_ && next.id != failed_id_) {
      startDecode(next.cloud, next.id);
    }
  }
}

void PointCloudLayer::refreshNow() {
  // Decode at the latest delivered tracker time, else at the first sample's
  // stamp (captured in attach). Presence is explicit — 0 ns is a valid stamp,
  // so a sim-time dataset starting at t=0 still refreshes.
  if (decoded_at_ns_.has_value()) {
    renderAt(PJ::toRaw(*decoded_at_ns_));
  } else if (ts_first_.has_value()) {
    renderAt(*ts_first_);
  }
}

void PointCloudLayer::onDatasetAboutToBeReplaced(PJ::DatasetId dataset_id) {
  if (ctx_.session == nullptr || ctx_.session->objectStore().descriptor(topic_id_).dataset_id != dataset_id) {
    return;
  }
  // replaceDataset keeps the ObjectTopicId stable while swapping the bytes, so a
  // (timestamp, byte size) key can alias new content: the decode cache could serve
  // stale points and failed_id_ would permanently refuse a now-valid sample.
  decoded_cache_.reset();
  decoded_cache_id_ = {};
  failed_id_ = {};
  last_pushed_id_ = {};
  last_pushed_color_field_.clear();
  pending_.reset();
  wanted_ = {};
  if (inflight_ != SampleId{}) {
    drop_inflight_result_ = true;  // the in-flight worker is decoding pre-reload bytes
  }
  // The signal precedes the swap and replaceDataset runs no event loop, so the
  // repaint scheduled here paints after the new bytes are in place.
  tracker_dirty_ = true;
  emit repaintRequested();
}

}  // namespace pj::scene3d
