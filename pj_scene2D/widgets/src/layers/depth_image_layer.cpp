// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/depth_image_layer.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <algorithm>
#include <memory>

#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_core/media_source.h"
#include "pj_widgets/DoubleScrubber.h"

namespace PJ {

namespace {
QString colormapName(Colormap colormap) {
  switch (colormap) {
    case Colormap::kTurbo:
      return QStringLiteral("turbo");
    case Colormap::kViridis:
      return QStringLiteral("viridis");
    case Colormap::kPlasma:
      return QStringLiteral("plasma");
    case Colormap::kGrayscale:
      return QStringLiteral("grayscale");
  }
  return QStringLiteral("turbo");
}

Colormap parseColormap(const QString& value) {
  if (value == QStringLiteral("viridis")) {
    return Colormap::kViridis;
  }
  if (value == QStringLiteral("plasma")) {
    return Colormap::kPlasma;
  }
  if (value == QStringLiteral("grayscale")) {
    return Colormap::kGrayscale;
  }
  return Colormap::kTurbo;  // also maps legacy "jet" layouts to the default.
}
}  // namespace

DepthImageLayer::DepthImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, QStringLiteral("Depth"), parent) {}

QWidget* DepthImageLayer::createConfigWidget(QWidget* parent) {
  auto* widget = new QWidget(parent);
  auto* layout = new QFormLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);

  // Colormap row: combo + a right-aligned invert toggle, matching the 3D
  // pointcloud color config exactly (same invert.svg icon, style, and extent).
  static constexpr auto kToggleButtonQss =
      "QPushButton { border: 1px solid #8c8c8c; border-radius: 4px; padding: 2px 10px; background-color: "
      "palette(button); }"
      "QPushButton:hover { border-color: #5b8fd9; }"
      "QPushButton:checked { background-color: #b7d2f5; border-color: #5b8fd9; }";

  auto* colormap_row = new QWidget(widget);
  auto* colormap_row_layout = new QHBoxLayout(colormap_row);
  colormap_row_layout->setContentsMargins(0, 0, 0, 0);
  colormap_row_layout->setSpacing(6);

  auto* colormap = new QComboBox(colormap_row);
  colormap->addItem(tr("Turbo"), static_cast<int>(Colormap::kTurbo));
  colormap->addItem(tr("Viridis"), static_cast<int>(Colormap::kViridis));
  colormap->addItem(tr("Plasma"), static_cast<int>(Colormap::kPlasma));
  colormap->addItem(tr("Grayscale"), static_cast<int>(Colormap::kGrayscale));
  colormap->setCurrentIndex(colormap->findData(static_cast<int>(colormap_)));
  colormap_row_layout->addWidget(colormap, 1);

  auto* invert_btn = new QPushButton(colormap_row);
  invert_btn->setCheckable(true);
  invert_btn->setChecked(invert_);
  invert_btn->setFocusPolicy(Qt::NoFocus);
  invert_btn->setStyleSheet(kToggleButtonQss);
  invert_btn->setIcon(QIcon(QStringLiteral(":/resources/svg/invert.svg")));
  invert_btn->setIconSize(QSize(20, 20));
  // Match the standard icon-button extent used across the app (icon 20 + padding 4).
  invert_btn->setFixedSize(24, 24);
  invert_btn->setToolTip(tr("Invert colormap"));
  colormap_row_layout->addWidget(invert_btn, 0);

  layout->addRow(tr("Colormap"), colormap_row);

  auto* near_spin = new PJ::DoubleScrubber(widget);
  near_spin->setRange(0.0, 100000.0);
  near_spin->setDecimals(3);
  near_spin->setSingleStep(0.1);
  near_spin->setSuffix(QStringLiteral(" m"));
  near_spin->setValue(near_m_);
  layout->addRow(tr("Near"), near_spin);

  auto* far_spin = new PJ::DoubleScrubber(widget);
  far_spin->setRange(0.0, 100000.0);
  far_spin->setDecimals(3);
  far_spin->setSingleStep(0.1);
  far_spin->setSuffix(QStringLiteral(" m"));
  far_spin->setValue(far_m_);
  layout->addRow(tr("Far"), far_spin);

  connect(colormap, &QComboBox::currentIndexChanged, this, [this, colormap](int) {
    setColormap(static_cast<Colormap>(colormap->currentData().toInt()));
  });
  connect(invert_btn, &QPushButton::toggled, this, [this](bool on) { setInvert(on); });
  connect(near_spin, &PJ::DoubleScrubber::valueChanged, this, [this, far_spin](double value) {
    setRange(static_cast<float>(value), static_cast<float>(far_spin->value()));
  });
  connect(far_spin, &PJ::DoubleScrubber::valueChanged, this, [this, near_spin](double value) {
    setRange(static_cast<float>(near_spin->value()), static_cast<float>(value));
  });

  return widget;
}

std::unique_ptr<MediaSource> DepthImageLayer::createMediaSource(const SceneLayerContext& ctx) {
  auto* session = ctx.session;
  auto* store = objectStore();
  if (session == nullptr || store == nullptr) {
    depth_source_ = nullptr;
    return nullptr;
  }

  // Real depth (e.g. ROS sensor_msgs/Image) is decoded via the topic's parser,
  // exactly like ImageLayer; only canonical pj_image_v1 producers have no parser.
  const auto binding = session->parserBindingForObjectTopic(topicId());
  std::unique_ptr<DepthPipelineSource> source;
  if (binding.parser != nullptr) {
    source = std::make_unique<DepthPipelineSource>(store, topicId(), binding.parser, binding.mutex, binding.keepalive);
  } else {
    source = std::make_unique<DepthPipelineSource>(store, topicId());
  }
  depth_source_ = source.get();
  applyTo(*source);
  // The decode runs on a worker thread now; repaint when a frame is ready.
  source->setFrameReadyCallback(makeQueuedRepaintCallback());
  return source;
}

void DepthImageLayer::onBeforeDetach() {
  depth_source_ = nullptr;
}

void DepthImageLayer::applyTo(DepthPipelineSource& source) const {
  source.setColormap(static_cast<uint8_t>(colormap_));  // core takes an opaque colormap id
  source.setInvert(invert_);
  source.setRange(near_m_, far_m_);  // always manual (the per-frame auto-range was dropped)
  source.setOpacity(opacity_);
}

void DepthImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(QStringLiteral("colormap"), colormapName(colormap_));
  element.setAttribute(QStringLiteral("invert"), invert_ ? QStringLiteral("true") : QStringLiteral("false"));
  element.setAttribute(QStringLiteral("near_m"), QString::number(near_m_, 'g', 9));
  element.setAttribute(QStringLiteral("far_m"), QString::number(far_m_, 'g', 9));
  element.setAttribute(QStringLiteral("opacity"), QString::number(opacity_, 'g', 9));
}

bool DepthImageLayer::loadOptions(const QDomElement& element) {
  colormap_ = parseColormap(element.attribute(QStringLiteral("colormap"), colormapName(colormap_)));
  invert_ = element.attribute(QStringLiteral("invert"), invert_ ? QStringLiteral("true") : QStringLiteral("false")) ==
            QStringLiteral("true");

  bool ok = false;
  const float near_m = element.attribute(QStringLiteral("near_m"), QString::number(near_m_)).toFloat(&ok);
  if (ok) {
    near_m_ = near_m;
  }
  ok = false;
  const float far_m = element.attribute(QStringLiteral("far_m"), QString::number(far_m_)).toFloat(&ok);
  if (ok) {
    far_m_ = far_m;
  }
  ok = false;
  const float opacity = element.attribute(QStringLiteral("opacity"), QString::number(opacity_)).toFloat(&ok);
  if (ok) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  }
  applyOptions();
  return true;
}

void DepthImageLayer::applyOptions() {
  if (depth_source_ == nullptr) {
    return;
  }
  applyTo(*depth_source_);
  // Colormap/range/invert are GPU uniforms carried on the frame's DepthColorParams
  // (not baked into the pixels). The setters above invalidate the worker so it
  // re-emits a frame with the new params; re-apply the current tracker time so that
  // re-emit happens now instead of on the next tick.
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    depth_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void DepthImageLayer::setColormap(Colormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  applyOptions();
}

void DepthImageLayer::setInvert(bool invert) {
  if (invert_ == invert) {
    return;
  }
  invert_ = invert;
  applyOptions();
}

void DepthImageLayer::setRange(float near_m, float far_m) {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  near_m_ = near_m;
  far_m_ = far_m;
  applyOptions();
}

}  // namespace PJ
