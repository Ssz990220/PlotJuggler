// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene2d_widgets/layers/depth_image_layer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QSignalBlocker>
#include <algorithm>
#include <memory>

#include "pj_scene2d_core/media_source.h"

namespace PJ {

namespace {
QString colormapName(DepthColormap colormap) {
  switch (colormap) {
    case DepthColormap::kJet:
      return QStringLiteral("jet");
    case DepthColormap::kTurbo:
      return QStringLiteral("turbo");
  }
  return QStringLiteral("turbo");
}

DepthColormap parseColormap(const QString& value) {
  return value == QStringLiteral("jet") ? DepthColormap::kJet : DepthColormap::kTurbo;
}
}  // namespace

DepthImageLayer::DepthImageLayer(
    ObjectTopicId topic_id, sdk::BuiltinObjectType object_type, const QString& display_name, QObject* parent)
    : Scene2DLayer(topic_id, object_type, display_name, QStringLiteral("Depth"), parent) {}

QWidget* DepthImageLayer::createConfigWidget(QWidget* parent) {
  auto* widget = new QWidget(parent);
  auto* layout = new QFormLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* colormap = new QComboBox(widget);
  colormap->addItem(QStringLiteral("Turbo"), QVariant::fromValue(0));
  colormap->addItem(QStringLiteral("Jet"), QVariant::fromValue(1));
  colormap->setCurrentIndex(colormap_ == DepthColormap::kJet ? 1 : 0);
  layout->addRow(tr("Colormap"), colormap);

  auto* auto_range = new QCheckBox(widget);
  auto_range->setChecked(auto_range_);
  layout->addRow(tr("Auto range"), auto_range);

  auto* near_spin = new QDoubleSpinBox(widget);
  near_spin->setRange(0.0, 100000.0);
  near_spin->setDecimals(3);
  near_spin->setSingleStep(0.1);
  near_spin->setSuffix(QStringLiteral(" m"));
  near_spin->setValue(near_m_);
  near_spin->setEnabled(!auto_range_);
  layout->addRow(tr("Near"), near_spin);

  auto* far_spin = new QDoubleSpinBox(widget);
  far_spin->setRange(0.0, 100000.0);
  far_spin->setDecimals(3);
  far_spin->setSingleStep(0.1);
  far_spin->setSuffix(QStringLiteral(" m"));
  far_spin->setValue(far_m_);
  far_spin->setEnabled(!auto_range_);
  layout->addRow(tr("Far"), far_spin);

  auto* opacity_spin = new QDoubleSpinBox(widget);
  opacity_spin->setRange(0.0, 1.0);
  opacity_spin->setDecimals(2);
  opacity_spin->setSingleStep(0.05);
  opacity_spin->setValue(opacity_);
  layout->addRow(tr("Opacity"), opacity_spin);

  connect(colormap, &QComboBox::currentIndexChanged, this, [this](int index) {
    setColormap(index == 1 ? DepthColormap::kJet : DepthColormap::kTurbo);
  });
  connect(auto_range, &QCheckBox::toggled, this, [this, near_spin, far_spin](bool enabled) {
    near_spin->setEnabled(!enabled);
    far_spin->setEnabled(!enabled);
    setAutoRange(enabled);
  });
  connect(near_spin, &QDoubleSpinBox::valueChanged, this, [this, far_spin](double value) {
    setRange(static_cast<float>(value), static_cast<float>(far_spin->value()));
  });
  connect(far_spin, &QDoubleSpinBox::valueChanged, this, [this, near_spin](double value) {
    setRange(static_cast<float>(near_spin->value()), static_cast<float>(value));
  });
  connect(opacity_spin, &QDoubleSpinBox::valueChanged, this, [this](double value) {
    setOpacity(static_cast<float>(value));
  });

  return widget;
}

std::unique_ptr<MediaSource> DepthImageLayer::createMediaSource(const SceneLayerContext& /*ctx*/) {
  auto* store = objectStore();
  if (store == nullptr) {
    depth_source_ = nullptr;
    return nullptr;
  }
  auto source = std::make_unique<DepthPipelineSource>(store, topicId());
  depth_source_ = source.get();
  applyTo(*source);
  return source;
}

void DepthImageLayer::onBeforeDetach() {
  depth_source_ = nullptr;
}

void DepthImageLayer::applyTo(DepthPipelineSource& source) const {
  source.setColormap(colormap_);
  source.setAutoRange(auto_range_);
  if (!auto_range_) {
    source.setRange(near_m_, far_m_);
  }
  source.setOpacity(opacity_);
}

void DepthImageLayer::saveOptions(QDomElement& element) const {
  element.setAttribute(QStringLiteral("colormap"), colormapName(colormap_));
  element.setAttribute(QStringLiteral("auto_range"), auto_range_ ? QStringLiteral("true") : QStringLiteral("false"));
  element.setAttribute(QStringLiteral("near_m"), QString::number(near_m_, 'g', 9));
  element.setAttribute(QStringLiteral("far_m"), QString::number(far_m_, 'g', 9));
  element.setAttribute(QStringLiteral("opacity"), QString::number(opacity_, 'g', 9));
}

bool DepthImageLayer::loadOptions(const QDomElement& element) {
  colormap_ = parseColormap(element.attribute(QStringLiteral("colormap"), colormapName(colormap_)));
  auto_range_ =
      element.attribute(QStringLiteral("auto_range"), auto_range_ ? QStringLiteral("true") : QStringLiteral("false")) ==
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
  // Colormap/range/opacity are baked into the decoded frame. The setters above
  // invalidate the source's cache, but only setTimestamp() re-decodes — so
  // re-apply the current tracker time to make the change visible immediately
  // instead of on the next tick.
  if (const auto ts = lastTrackerTimeNs(); ts.has_value()) {
    depth_source_->setTimestamp(*ts);
  }
  emit repaintRequested();
}

void DepthImageLayer::setColormap(DepthColormap colormap) {
  if (colormap_ == colormap) {
    return;
  }
  colormap_ = colormap;
  applyOptions();
}

void DepthImageLayer::setAutoRange(bool enabled) {
  if (auto_range_ == enabled) {
    return;
  }
  auto_range_ = enabled;
  applyOptions();
}

void DepthImageLayer::setRange(float near_m, float far_m) {
  if (far_m < near_m) {
    std::swap(near_m, far_m);
  }
  near_m_ = near_m;
  far_m_ = far_m;
  if (!auto_range_) {
    applyOptions();
  }
}

void DepthImageLayer::setOpacity(float opacity) {
  opacity = std::clamp(opacity, 0.0f, 1.0f);
  if (opacity_ == opacity) {
    return;
  }
  opacity_ = opacity;
  applyOptions();
}

}  // namespace PJ
