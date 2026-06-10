// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/Scene3DConfigPanel.h"

#include <QVBoxLayout>
#include <cstdint>
#include <utility>
#include <vector>

#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/SectionHeaderBand.h"

namespace PJ {

namespace {

[[nodiscard]] ObjectTopicId topicFromRowId(qint64 id) {
  ObjectTopicId topic_id;
  topic_id.id = static_cast<uint32_t>(id);
  return topic_id;
}

[[nodiscard]] LayerRow rowFromLayerInfo(const SceneLayerInfo& info) {
  return LayerRow{
      .id = static_cast<qint64>(info.topic_id.id),
      .name = info.display_name,
      .visible = info.visible,
  };
}

}  // namespace

Scene3DConfigPanel::Scene3DConfigPanel(QWidget* parent) : QWidget(parent) {
  // Zero outer margins so the section header bands span edge-to-edge, like
  // the plotting panel's Curve Width / Curve Style strips; each content block
  // under a band re-adds its own inset.
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  root->addWidget(new SectionHeaderBand(tr("Topics"), this));
  auto* topics_host = new QWidget(this);
  auto* topics_layout = new QVBoxLayout(topics_host);
  topics_layout->setContentsMargins(8, 4, 8, 4);
  layer_list_ = new LayerListView(topics_host);
  topics_layout->addWidget(layer_list_);
  root->addWidget(topics_host);

  root->addWidget(new SectionHeaderBand(tr("Settings"), this));
  auto* settings_host = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_host);
  settings_layout->setContentsMargins(8, 4, 8, 4);
  config_host_ = new ConfigPanelHost(settings_host);
  settings_layout->addWidget(config_host_);
  settings_layout->addStretch(1);
  root->addWidget(settings_host, /*stretch=*/1);

  connect(layer_list_, &LayerListView::selectionChanged, this, &Scene3DConfigPanel::onLayerSelectionChanged);
  connect(layer_list_, &LayerListView::visibilityToggled, this, [this](qint64 id, bool visible) {
    if (bound_dock_ != nullptr) {
      bound_dock_->setLayerVisible(topicFromRowId(id), visible);
    }
  });
  connect(layer_list_, &LayerListView::removeRequested, this, [this](qint64 id) {
    if (bound_dock_ != nullptr) {
      bound_dock_->removeTopic(topicFromRowId(id));
    }
  });
  connect(layer_list_, &LayerListView::reordered, this, [this](const std::vector<qint64>& ids) {
    if (bound_dock_ != nullptr) {
      bound_dock_->reorderLayers(topicOrderFromIds(ids));
    }
  });

  updateSelectedLayerPane();
}

void Scene3DConfigPanel::bindDock(Scene3DDockWidget* dock) {
  if (bound_dock_.data() == dock) {
    return;
  }
  disconnectFromDock();
  bound_dock_ = dock;

  layer_list_->clearRows();
  config_host_->clear();

  if (dock == nullptr) {
    updateSelectedLayerPane();
    return;
  }

  rebuildLayerList();

  connect(dock, &SceneDockWidget::layerAdded, this, &Scene3DConfigPanel::onLayerAdded);
  connect(dock, &SceneDockWidget::layerRemoved, this, &Scene3DConfigPanel::onLayerRemoved);
  connect(dock, &SceneDockWidget::layerVisibilityChanged, this, &Scene3DConfigPanel::onLayerVisibilityChanged);
  connect(dock, &SceneDockWidget::layerWarningChanged, this, &Scene3DConfigPanel::onLayerWarningChanged);
}

void Scene3DConfigPanel::disconnectFromDock() {
  if (bound_dock_ != nullptr) {
    disconnect(bound_dock_.data(), nullptr, this, nullptr);
  }
  bound_dock_ = nullptr;
}

void Scene3DConfigPanel::rebuildLayerList() {
  if (bound_dock_ == nullptr) {
    layer_list_->clearRows();
    updateSelectedLayerPane();
    return;
  }

  std::vector<LayerRow> rows;
  const auto layers = bound_dock_->layers();
  rows.reserve(layers.size());
  for (const SceneLayerInfo& info : layers) {
    rows.push_back(rowFromLayerInfo(info));
  }
  layer_list_->setRows(rows);

  for (const SceneLayerInfo& info : layers) {
    const auto warning = bound_dock_->orphanState(info.topic_id);
    layer_list_->setRowWarning(static_cast<qint64>(info.topic_id.id), warning.is_orphan, warning.reason);
  }
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerSelectionChanged() {
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerAdded(ObjectTopicId topic_id) {
  if (bound_dock_ == nullptr) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(topic_id);
  if (layer == nullptr) {
    return;
  }
  const SceneLayerInfo info = layer->info();
  layer_list_->addRow(rowFromLayerInfo(info));
  const auto warning = bound_dock_->orphanState(topic_id);
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warning.is_orphan, warning.reason);
}

void Scene3DConfigPanel::onLayerRemoved(ObjectTopicId topic_id) {
  layer_list_->removeRow(static_cast<qint64>(topic_id.id));
  updateSelectedLayerPane();
}

void Scene3DConfigPanel::onLayerVisibilityChanged(ObjectTopicId topic_id, bool visible) {
  layer_list_->setRowVisible(static_cast<qint64>(topic_id.id), visible);
}

void Scene3DConfigPanel::onLayerWarningChanged(ObjectTopicId topic_id, bool warn, const QString& reason) {
  layer_list_->setRowWarning(static_cast<qint64>(topic_id.id), warn, reason);
}

void Scene3DConfigPanel::onStylesheetChanged(QString theme) {
  layer_list_->setTheme(std::move(theme));
}

void Scene3DConfigPanel::updateSelectedLayerPane() {
  config_host_->clear();
  if (bound_dock_ == nullptr) {
    return;
  }
  const auto selected = selectedTopicId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = bound_dock_->layerFor(*selected);
  if (layer == nullptr) {
    return;
  }
  config_host_->setConfigWidget(layer->createConfigWidget(config_host_));
}

std::optional<ObjectTopicId> Scene3DConfigPanel::selectedTopicId() const {
  if (layer_list_ == nullptr) {
    return std::nullopt;
  }
  const auto id = layer_list_->currentId();
  if (!id.has_value()) {
    return std::nullopt;
  }
  return topicFromRowId(*id);
}

std::vector<ObjectTopicId> Scene3DConfigPanel::topicOrderFromIds(const std::vector<qint64>& ids) const {
  std::vector<ObjectTopicId> ordered;
  ordered.reserve(ids.size());
  for (const qint64 id : ids) {
    ordered.push_back(topicFromRowId(id));
  }
  return ordered;
}

}  // namespace PJ
