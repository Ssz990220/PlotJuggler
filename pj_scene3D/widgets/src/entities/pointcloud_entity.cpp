// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/entities/pointcloud_entity.h"

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
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <string_view>

#include "pj_base/builtin/point_cloud.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/pointcloud.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/DoubleScrubber.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcPointCloudEntity, "pj.scene3d.entity.pointcloud")

using PJ::Span;
using PJ::sdk::PayloadView;
using PJ::sdk::PointCloud;
using PJ::sdk::PointField;

float readFloat32At(const uint8_t* p) {
  uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                  (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  return std::bit_cast<float>(bits);
}
double readFloat64At(const uint8_t* p) {
  uint64_t bits = 0;
  for (int i = 0; i < 8; ++i) {
    bits |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return std::bit_cast<double>(bits);
}
float readScalarAt(const uint8_t* p, PointField::Datatype dt) {
  using DT = PointField::Datatype;
  switch (dt) {
    case DT::kInt8:
      return static_cast<float>(std::bit_cast<int8_t>(*p));
    case DT::kUint8:
      return static_cast<float>(*p);
    case DT::kInt16: {
      uint16_t bits = static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
      return static_cast<float>(std::bit_cast<int16_t>(bits));
    }
    case DT::kUint16:
      return static_cast<float>(static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8));
    case DT::kInt32: {
      uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                      (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
      return static_cast<float>(std::bit_cast<int32_t>(bits));
    }
    case DT::kUint32: {
      uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                      (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
      return static_cast<float>(bits);
    }
    case DT::kFloat32:
      return readFloat32At(p);
    case DT::kFloat64:
      return static_cast<float>(readFloat64At(p));
    case DT::kUnknown:
    default:
      return 0.0f;
  }
}

const PointField* findField(const std::vector<PointField>& fields, std::string_view name) {
  const auto it =
      std::find_if(fields.begin(), fields.end(), [name](const auto& f) { return std::string_view(f.name) == name; });
  return it == fields.end() ? nullptr : &*it;
}

DecodedPointCloud convertCanonical(const PointCloud& src, std::string_view scalar_field) {
  DecodedPointCloud out;
  out.stamp = std::chrono::nanoseconds(src.timestamp_ns);
  out.frame_id = src.frame_id;

  if (src.is_bigendian) {
    qCWarning(lcPointCloudEntity) << "PointCloud is big-endian; not supported, dropping";
    return out;
  }
  const std::size_t n = static_cast<std::size_t>(src.width) * static_cast<std::size_t>(src.height);
  if (n == 0 || src.point_step == 0 || src.data.empty()) {
    return out;
  }
  const std::size_t step = src.point_step;
  if (src.data.size() < n * step) {
    qCWarning(lcPointCloudEntity) << "PointCloud data buffer too small:" << src.data.size() << "<" << n * step;
    return out;
  }
  const PointField* xf = findField(src.fields, "x");
  const PointField* yf = findField(src.fields, "y");
  const PointField* zf = findField(src.fields, "z");
  if (!xf || !yf || !zf) {
    qCWarning(lcPointCloudEntity) << "PointCloud missing x/y/z fields";
    return out;
  }

  auto readCoord = [&](const PointField* f, const uint8_t* base) {
    if (f->datatype == PointField::Datatype::kFloat32) {
      return readFloat32At(base + f->offset);
    }
    if (f->datatype == PointField::Datatype::kFloat64) {
      return static_cast<float>(readFloat64At(base + f->offset));
    }
    return 0.0f;
  };

  out.positions.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const uint8_t* base = src.data.data() + i * step;
    out.positions.push_back(glm::vec3{readCoord(xf, base), readCoord(yf, base), readCoord(zf, base)});
  }

  if (!scalar_field.empty()) {
    const PointField* sf = findField(src.fields, scalar_field);
    if (sf != nullptr) {
      out.scalar.reserve(n);
      for (std::size_t i = 0; i < n; ++i) {
        const uint8_t* base = src.data.data() + i * step;
        out.scalar.push_back(readScalarAt(base + sf->offset, sf->datatype));
      }
      out.scalar_field_name = std::string(scalar_field);
    }
  }
  return out;
}

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

PointCloudEntity::PointCloudEntity(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DEntity(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {
  // Push entity defaults to the pass at construction so the first paint
  // already reflects them (entity defaults are the design-spec values, not
  // the pass's "minimum visual change" defaults).
  cloud_pass_.setShape(shape_);
  cloud_pass_.setSizeMeters(size_meters_);
  cloud_pass_.setSizePixels(size_pixels_);
  cloud_pass_.setColorType(color_type_);
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  cloud_pass_.setColormap(colormap_);
  cloud_pass_.setInvertLut(invert_lut_);
}

PointCloudEntity::~PointCloudEntity() = default;

Scene3DEntityInfo PointCloudEntity::info() const {
  return Scene3DEntityInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kPointCloud,
      .display_name = display_name_,
      .family_name = QStringLiteral("PointCloud"),
      .visible = visible_,
  };
}

std::pair<int64_t, int64_t> PointCloudEntity::timeRangeNs() const {
  return {ts_first_, ts_last_};
}

QStringList PointCloudEntity::fallbackFrames() const {
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  return out;
}

QString PointCloudEntity::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement PointCloudEntity::xmlSaveState(QDomDocument& doc) const {
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

bool PointCloudEntity::xmlLoadState(const QDomElement& element) {
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

bool PointCloudEntity::attach(const Scene3DEntityContext& ctx) {
  if (ctx.session == nullptr) {
    qCWarning(lcPointCloudEntity) << "attach: session is null";
    return false;
  }
  ctx_ = ctx;
  parser_ = ctx_.session->parserForObjectTopic(topic_id_);
  if (parser_ == nullptr) {
    qCWarning(lcPointCloudEntity) << "attach: no parser for topic_id=" << topic_id_.id;
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  if (store.entryCount(topic_id_) > 0) {
    const auto range = store.timeRange(topic_id_);
    ts_first_ = range.first;
    ts_last_ = range.second;
  }
  if (!bootstrap()) {
    qCWarning(lcPointCloudEntity) << "attach: bootstrap failed for topic_id=" << topic_id_.id;
    // Continue anyway — render will silently skip until a sample arrives.
  }
  if (ts_first_ != 0) {
    renderAt(ts_first_);
  }
  return true;
}

void PointCloudEntity::detach() {
  parser_ = nullptr;
  ctx_ = {};
  cloud_pass_.setActiveCloud(nullptr);
}

void PointCloudEntity::setFixedFrame(const QString& frame) {
  if (fixed_frame_ == frame) {
    return;
  }
  fixed_frame_ = frame;
  // X/Y/Z color is world-frame anchored; switching the fixed-frame
  // invalidates the cached range. Refit immediately so a stopped
  // tracker still updates its colormap — otherwise the cloud paints
  // with the previous frame's normalization until the next tick.
  range_dirty_ = true;
  refreshNow();
}

void PointCloudEntity::setTrackerTime(std::chrono::nanoseconds time) {
  decoded_at_ns_ = time;
  if (visible_) {
    renderAt(time.count());
  }
}

void PointCloudEntity::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  cloud_pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Un-hiding decodes at the current tracker time so the entity isn't
  // stuck painting the geometry it had at the moment of hiding (the
  // dock skips hidden entities in onTrackerTime, so any tracker moves
  // while hidden are not reflected in the pass's VBO).
  if (visible) {
    refreshNow();
  }
  emit repaintRequested();
}

void PointCloudEntity::initializeGL() {
  cloud_pass_.initializeGL();
}

void PointCloudEntity::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  cloud_pass_.render(view_params, frame_ctx);
}

QWidget* PointCloudEntity::createConfigWidget(QWidget* parent) {
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

  // ---- User → entity wires -------------------------------------------------

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

  QObject::connect(invert_btn, &QPushButton::toggled, this, &PointCloudEntity::setInvertLut);

  const auto push_manual_range = [this, range_min_spin, range_max_spin]() {
    setManualRange(static_cast<float>(range_min_spin->value()), static_cast<float>(range_max_spin->value()));
  };
  QObject::connect(range_min_spin, &PJ::DoubleScrubber::valueChanged, this, push_manual_range);
  QObject::connect(range_max_spin, &PJ::DoubleScrubber::valueChanged, this, push_manual_range);

  // ---- Entity → widget wires (out-of-band changes / auto-range refresh) ----

  QPointer<QComboBox> safe_type(color_type_combo);
  QObject::connect(this, &PointCloudEntity::colorFieldsChanged, container, [safe_type, rebuild_color_type_combo]() {
    if (!safe_type) {
      return;
    }
    rebuild_color_type_combo();
  });

  QPointer<PJ::DoubleScrubber> safe_min(range_min_spin);
  QPointer<PJ::DoubleScrubber> safe_max(range_max_spin);
  QObject::connect(this, &PointCloudEntity::autoRangeComputed, container, [safe_min, safe_max](float lo, float hi) {
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

void PointCloudEntity::setColorField(const QString& field) {
  const std::string new_field = field.toStdString();
  if (color_field_ == new_field) {
    return;
  }
  color_field_ = new_field;
  range_dirty_ = true;
  emit currentColorFieldChanged(field);
  refreshNow();
}

void PointCloudEntity::setShape(PointcloudRenderPass::Shape shape) {
  if (shape_ == shape) {
    return;
  }
  shape_ = shape;
  cloud_pass_.setShape(shape_);
  emit repaintRequested();
}

void PointCloudEntity::setSizeMeters(float meters) {
  if (size_meters_ == meters) {
    return;
  }
  size_meters_ = meters;
  cloud_pass_.setSizeMeters(size_meters_);
  emit repaintRequested();
}

void PointCloudEntity::setSizePixels(int pixels) {
  if (size_pixels_ == pixels) {
    return;
  }
  size_pixels_ = pixels;
  cloud_pass_.setSizePixels(size_pixels_);
  emit repaintRequested();
}

void PointCloudEntity::setColorType(PointcloudRenderPass::ColorType type) {
  if (color_type_ == type) {
    return;
  }
  color_type_ = type;
  cloud_pass_.setColorType(color_type_);
  emit repaintRequested();
}

void PointCloudEntity::setSolidColor(QColor color) {
  if (solid_color_ == color) {
    return;
  }
  solid_color_ = color;
  cloud_pass_.setSolidColor(glm::vec3(solid_color_.redF(), solid_color_.greenF(), solid_color_.blueF()));
  emit repaintRequested();
}

void PointCloudEntity::setColormap(PointcloudRenderPass::Colormap cm) {
  if (colormap_ == cm) {
    return;
  }
  colormap_ = cm;
  cloud_pass_.setColormap(colormap_);
  emit repaintRequested();
}

void PointCloudEntity::setInvertLut(bool invert) {
  if (invert_lut_ == invert) {
    return;
  }
  invert_lut_ = invert;
  cloud_pass_.setInvertLut(invert_lut_);
  emit repaintRequested();
}

void PointCloudEntity::setAutoRange(bool enable) {
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
    // Pin the pass to whatever manual values the entity is currently
    // holding so the colormap doesn't snap to stale auto-computed bounds.
    cloud_pass_.setColormapRange(manual_range_min_, manual_range_max_);
    emit repaintRequested();
  }
}

void PointCloudEntity::setManualRange(float min_value, float max_value) {
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

bool PointCloudEntity::bootstrap() {
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto first = store.at(topic_id_, 0);
  if (!first.has_value() || first->payload.bytes.empty()) {
    return false;
  }
  auto obj = parser_->parseObject(first->timestamp, first->payload);
  if (!obj.has_value()) {
    qCWarning(lcPointCloudEntity) << "bootstrap parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return false;
  }
  if (sdk_cloud->frame_id != source_frame_) {
    source_frame_ = sdk_cloud->frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
  }

  available_color_fields_.clear();
  for (const auto& f : sdk_cloud->fields) {
    if (f.name == "timestamp") {
      continue;
    }
    available_color_fields_.append(QString::fromStdString(f.name));
  }
  color_field_ = defaultColorField(available_color_fields_).toStdString();
  emit colorFieldsChanged(available_color_fields_);
  if (!color_field_.empty()) {
    emit currentColorFieldChanged(QString::fromStdString(color_field_));
  }
  if (!source_frame_.empty()) {
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

void PointCloudEntity::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr || parser_ == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  auto obj = parser_->parseObject(resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcPointCloudEntity) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }
  const auto* sdk_cloud = std::any_cast<PointCloud>(&obj->object);
  if (sdk_cloud == nullptr) {
    return;
  }
  // A topic's frame_id can change across samples (rare in practice, but
  // common when a recording bridges across config reloads). Track it so
  // the dock's orphan check stays accurate and the fallback-frames list
  // sees the new frame even between bootstrap calls.
  if (sdk_cloud->frame_id != source_frame_) {
    source_frame_ = sdk_cloud->frame_id;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  auto decoded = convertCanonical(*sdk_cloud, color_field_);

  if (!decoded.scalar.empty() && auto_range_) {
    const bool is_spatial = color_field_ == "x" || color_field_ == "y" || color_field_ == "z";
    if (range_dirty_ || is_spatial) {
      const auto [lo, hi] = computeScalarRange(decoded.scalar);
      cloud_pass_.setColormapRange(lo, hi);
      range_dirty_ = false;
      emit autoRangeComputed(lo, hi);
    }
  }

  cloud_pass_.setActiveCloud(std::make_shared<DecodedPointCloud>(std::move(decoded)));
  emit repaintRequested();
}

void PointCloudEntity::refreshNow() {
  // Decode at the latest tracker time if known, else at ts_first_ (which
  // is set by bootstrap before any tracker tick fires).
  const int64_t t = decoded_at_ns_.count() != 0 ? decoded_at_ns_.count() : ts_first_;
  if (t != 0) {
    renderAt(t);
  }
}

}  // namespace pj::scene3d
