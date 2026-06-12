// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/scene_entities_layer.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLoggingCategory>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <utility>

#include "pj_base/builtin/scene_entities.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/scene_entities_decode.h"
#include "pj_scene3d_widgets/parse_locked.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/DoubleScrubber.h"

namespace pj::scene3d {

namespace {
Q_LOGGING_CATEGORY(lcSceneEntitiesLayer, "pj.scene3d.entity.markers")

// The source frame of a batch is the message-level frame: the frame_id of its
// first entity (per-entity frame overrides are a v2 refinement).
std::string batchSourceFrame(const PJ::sdk::SceneEntities& batch) {
  return batch.entities.empty() ? std::string{} : batch.entities.front().frame_id;
}

// Repaint a swatch button so its face shows the current override color.
void paintSwatch(QPushButton* button, const QColor& color) {
  button->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #555; border-radius: 3px;")
                            .arg(color.name(QColor::HexRgb)));
}
}  // namespace

SceneEntitiesLayer::SceneEntitiesLayer(PJ::ObjectTopicId topic_id, QString display_name, QObject* parent)
    : Scene3DLayer(parent), topic_id_(topic_id), display_name_(std::move(display_name)) {}

SceneEntitiesLayer::~SceneEntitiesLayer() = default;

PJ::SceneLayerInfo SceneEntitiesLayer::info() const {
  return PJ::SceneLayerInfo{
      .topic_id = topic_id_,
      .object_type = PJ::sdk::BuiltinObjectType::kSceneEntities,
      .display_name = display_name_,
      .family_name = QStringLiteral("Markers"),
      .visible = visible_,
  };
}

PJ::Range<PJ::Timepoint> SceneEntitiesLayer::timeRange() const {
  return PJ::liveTopicTimeRange(ctx_.session != nullptr ? &ctx_.session->objectStore() : nullptr, topic_id_);
}

QStringList SceneEntitiesLayer::fallbackFrames() const {
  QStringList out;
  if (!source_frame_.empty()) {
    out.append(QString::fromStdString(source_frame_));
  }
  return out;
}

QString SceneEntitiesLayer::sourceFrame() const {
  return QString::fromStdString(source_frame_);
}

QDomElement SceneEntitiesLayer::xmlSaveState(QDomDocument& doc) const {
  QDomElement el = doc.createElement(QStringLiteral("markers"));
  el.setAttribute(QStringLiteral("opacity"), QString::number(static_cast<double>(overrides_.opacity), 'g', 6));
  el.setAttribute(
      QStringLiteral("color_override"), overrides_.color_override ? QStringLiteral("true") : QStringLiteral("false"));
  el.setAttribute(QStringLiteral("override_color"), overrideColor().name(QColor::HexRgb));
  el.setAttribute(QStringLiteral("wireframe"), overrides_.wireframe ? QStringLiteral("true") : QStringLiteral("false"));
  return el;
}

bool SceneEntitiesLayer::xmlLoadState(const QDomElement& element) {
  if (element.isNull() || element.tagName() != QStringLiteral("markers")) {
    return false;
  }
  bool ok = false;
  const float op = element.attribute(QStringLiteral("opacity"), QStringLiteral("1")).toFloat(&ok);
  if (ok) {
    setOpacity(op);
  }
  if (element.hasAttribute(QStringLiteral("override_color"))) {
    const QColor c(element.attribute(QStringLiteral("override_color")));
    if (c.isValid()) {
      setOverrideColor(c);
    }
  }
  setColorOverrideEnabled(element.attribute(QStringLiteral("color_override")) == QStringLiteral("true"));
  setWireframe(element.attribute(QStringLiteral("wireframe")) == QStringLiteral("true"));
  return true;
}

bool SceneEntitiesLayer::attach(const PJ::SceneLayerContext& ctx) {
  const auto& scene3d_ctx = static_cast<const Scene3DLayerContext&>(ctx);
  if (scene3d_ctx.session == nullptr) {
    qCWarning(lcSceneEntitiesLayer) << "attach: session is null";
    return false;
  }
  ctx_ = scene3d_ctx;
  if (!ctx_.session->parserBindingForObjectTopic(topic_id_)) {
    qCWarning(lcSceneEntitiesLayer) << "attach: no parser for topic_id=" << topic_id_.id;
    return false;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  if (store.entryCount(topic_id_) > 0) {
    ts_first_ = store.timeRange(topic_id_).first;
  }
  if (!bootstrap()) {
    qCWarning(lcSceneEntitiesLayer) << "attach: bootstrap failed for topic_id=" << topic_id_.id;
    // Continue anyway — render will silently skip until a sample arrives.
  }
  if (ts_first_ != 0) {
    renderAt(ts_first_);
  }
  return true;
}

void SceneEntitiesLayer::detach() {
  ctx_ = {};
  pass_.setActive(nullptr);
}

void SceneEntitiesLayer::setTrackerTime(PJ::Timepoint time) {
  decoded_at_ns_ = time;
  if (visible_) {
    renderAt(PJ::toRaw(time));
  }
}

void SceneEntitiesLayer::setVisible(bool visible) {
  if (visible_ == visible) {
    return;
  }
  visible_ = visible;
  pass_.setVisible(visible);
  emit visibilityChanged(visible);
  // Un-hiding re-decodes at the current playhead: the dock skips hidden entities
  // on tracker ticks, so the cached batch may be stale by the time we show again.
  if (visible) {
    refreshNow();
  }
  emit repaintRequested();
}

void SceneEntitiesLayer::initializeGL() {
  pass_.initializeGL();
}

void SceneEntitiesLayer::render(const ViewParams& view_params, const FrameContext& frame_ctx) {
  pass_.render(view_params, frame_ctx);
}

void SceneEntitiesLayer::releaseGL() {
  pass_.releaseGL();
}

bool SceneEntitiesLayer::bootstrap() {
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
    qCWarning(lcSceneEntitiesLayer) << "bootstrap parseObject failed:" << QString::fromStdString(obj.error());
    return false;
  }
  const auto* batch = std::any_cast<PJ::sdk::SceneEntities>(&obj->object);
  if (batch == nullptr) {
    return false;
  }
  const std::string frame = batchSourceFrame(*batch);
  if (frame != source_frame_) {
    source_frame_ = frame;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
  }
  if (!source_frame_.empty()) {
    emit fallbackFramesChanged(fallbackFrames());
  }
  return true;
}

void SceneEntitiesLayer::renderAt(int64_t time_ns) {
  if (ctx_.session == nullptr) {
    return;
  }
  PJ::ObjectStore& store = ctx_.session->objectStore();
  auto resolved = store.latestAt(topic_id_, time_ns);
  if (!resolved.has_value() || resolved->payload.bytes.empty()) {
    return;
  }
  const auto binding = ctx_.session->parserBindingForObjectTopic(topic_id_);
  if (!binding) {
    return;
  }
  auto obj = parseLocked(binding, resolved->timestamp, resolved->payload);
  if (!obj.has_value()) {
    qCWarning(lcSceneEntitiesLayer) << "renderAt parseObject failed:" << QString::fromStdString(obj.error());
    return;
  }
  const auto* batch = std::any_cast<PJ::sdk::SceneEntities>(&obj->object);
  if (batch == nullptr) {
    return;
  }
  const std::string frame = batchSourceFrame(*batch);
  if (frame != source_frame_) {
    source_frame_ = frame;
    emit sourceFrameChanged(QString::fromStdString(source_frame_));
    emit fallbackFramesChanged(fallbackFrames());
  }
  pass_.setActive(std::make_shared<const DecodedSceneEntities>(decodeSceneEntities(*batch)));
  emit repaintRequested();
}

void SceneEntitiesLayer::refreshNow() {
  const int64_t t = decoded_at_ns_ != PJ::Timepoint{} ? PJ::toRaw(decoded_at_ns_) : ts_first_;
  if (t != 0) {
    renderAt(t);
  }
}

void SceneEntitiesLayer::applyOverrides() {
  pass_.setOverrides(overrides_);
  emit repaintRequested();
}

void SceneEntitiesLayer::setOpacity(float opacity) {
  overrides_.opacity = opacity;
  applyOverrides();
}

void SceneEntitiesLayer::setColorOverrideEnabled(bool enabled) {
  overrides_.color_override = enabled;
  applyOverrides();
}

void SceneEntitiesLayer::setOverrideColor(QColor color) {
  overrides_.override_color = glm::vec4(
      static_cast<float>(color.redF()), static_cast<float>(color.greenF()), static_cast<float>(color.blueF()), 1.0F);
  applyOverrides();
}

void SceneEntitiesLayer::setWireframe(bool enabled) {
  overrides_.wireframe = enabled;
  applyOverrides();
}

QWidget* SceneEntitiesLayer::createConfigWidget(QWidget* parent) {
  auto* container = new QWidget(parent);
  auto* outer = new QVBoxLayout(container);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(6);
  auto* form = new QFormLayout();
  form->setContentsMargins(0, 0, 0, 0);
  form->setSpacing(6);
  outer->addLayout(form);

  // Opacity (always active): multiplies every primitive's alpha.
  auto* opacity_spin = new PJ::DoubleScrubber(container);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(static_cast<double>(overrides_.opacity));
  form->addRow(tr("Opacity:"), opacity_spin);
  QObject::connect(
      opacity_spin, &PJ::DoubleScrubber::valueChanged, this, [this](double v) { setOpacity(static_cast<float>(v)); });

  // Override color: a checkbox gating a swatch button. The marker protocol has
  // no override field — this is a viewer-only recolor of every primitive.
  auto* override_chk = new QCheckBox(tr("Override color"), container);
  override_chk->setChecked(overrides_.color_override);
  form->addRow(override_chk);

  // The swatch stays clickable regardless of the checkbox: picking a color
  // auto-enables the override (ticks the box) so the recolor is immediate —
  // otherwise the button looks dead until the user discovers the gate.
  auto* swatch = new QPushButton(container);
  swatch->setFixedWidth(48);
  paintSwatch(swatch, overrideColor());
  form->addRow(tr("Color:"), swatch);

  QObject::connect(override_chk, &QCheckBox::toggled, this, [this](bool on) { setColorOverrideEnabled(on); });
  QObject::connect(swatch, &QPushButton::clicked, this, [this, container, swatch, override_chk]() {
    auto* popup = new PJ::ColorPickerPopup(container);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setColor(overrideColor());
    QObject::connect(popup, &PJ::ColorPickerPopup::colorChanged, this, [this, swatch, override_chk](QColor c) {
      if (c.isValid()) {
        setOverrideColor(c);
        paintSwatch(swatch, c);
        // Picking a color implies the user wants it applied: tick the box
        // (which routes through setColorOverrideEnabled via its toggled slot).
        if (!override_chk->isChecked()) {
          override_chk->setChecked(true);
        }
      }
    });
    popup->move(swatch->mapToGlobal(QPoint(0, swatch->height() + 2)));
    popup->show();
  });

  // Wireframe (always active): draws mesh primitives as edges.
  auto* wire_chk = new QCheckBox(tr("Wireframe"), container);
  wire_chk->setChecked(overrides_.wireframe);
  form->addRow(wire_chk);
  QObject::connect(wire_chk, &QCheckBox::toggled, this, [this](bool on) { setWireframe(on); });

  return container;
}

}  // namespace pj::scene3d
