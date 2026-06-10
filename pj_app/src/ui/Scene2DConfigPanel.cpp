// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "ui/Scene2DConfigPanel.h"

#include <QVBoxLayout>
#include <utility>
#include <vector>

#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"
#include "pj_widgets/ConfigPanelHost.h"
#include "pj_widgets/LayerListView.h"
#include "pj_widgets/SectionHeaderBand.h"

namespace PJ {

namespace {
// SceneDockWidget keys layers by ObjectTopicId; LayerListView keys rows by an
// opaque qint64. The dock's topic id IS that opaque key.
[[nodiscard]] ObjectTopicId toTopicId(qint64 id) {
  ObjectTopicId tid;
  tid.id = static_cast<uint32_t>(id);
  return tid;
}
}  // namespace

Scene2DConfigPanel::Scene2DConfigPanel(QWidget* parent) : QWidget(parent) {
  // Zero outer margins so the section header bands span edge-to-edge, like
  // the plotting panel's Curve Width / Curve Style strips; each content block
  // under a band re-adds its own inset.
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  root->addWidget(new SectionHeaderBand(tr("Layers"), this));
  auto* layers_host = new QWidget(this);
  auto* layers_layout = new QVBoxLayout(layers_host);
  layers_layout->setContentsMargins(8, 4, 8, 4);
  list_ = new LayerListView(layers_host);
  layers_layout->addWidget(list_);
  root->addWidget(layers_host);

  root->addWidget(new SectionHeaderBand(tr("Settings"), this));
  auto* settings_host = new QWidget(this);
  auto* settings_layout = new QVBoxLayout(settings_host);
  settings_layout->setContentsMargins(8, 4, 8, 4);
  config_host_ = new ConfigPanelHost(settings_host);
  settings_layout->addWidget(config_host_);
  settings_layout->addStretch(1);
  root->addWidget(settings_host, /*stretch=*/1);

  connect(list_, &LayerListView::selectionChanged, this, &Scene2DConfigPanel::updateConfigPane);
  connect(list_, &LayerListView::visibilityToggled, this, [this](qint64 id, bool visible) {
    if (dock_ != nullptr) {
      dock_->setLayerVisible(toTopicId(id), visible);
    }
  });
  connect(list_, &LayerListView::removeRequested, this, [this](qint64 id) {
    if (dock_ != nullptr) {
      dock_->removeTopic(toTopicId(id));
    }
  });
  connect(list_, &LayerListView::reordered, this, [this](const std::vector<qint64>& ordered_ids) {
    if (dock_ == nullptr) {
      return;
    }
    std::vector<ObjectTopicId> ordered;
    ordered.reserve(ordered_ids.size());
    for (const qint64 id : ordered_ids) {
      ordered.push_back(toTopicId(id));
    }
    dock_->reorderLayers(ordered);
  });
}

void Scene2DConfigPanel::bindDock(SceneDockWidget* dock) {
  if (dock_.data() == dock) {
    return;
  }
  disconnectDock();
  dock_ = dock;

  list_->clearRows();
  config_host_->clear();

  if (dock == nullptr) {
    return;
  }

  rebuildList();

  connect(dock, &SceneDockWidget::layerAdded, this, [this](ObjectTopicId topic_id) {
    if (dock_ == nullptr) {
      return;
    }
    if (ISceneLayer* layer = dock_->layerFor(topic_id)) {
      const auto info = layer->info();
      list_->addRow(LayerRow{info.topic_id.id, info.display_name, info.visible});
    }
    updateConfigPane();
  });
  connect(dock, &SceneDockWidget::layerRemoved, this, [this](ObjectTopicId topic_id) {
    list_->removeRow(topic_id.id);
    updateConfigPane();
  });
  connect(dock, &SceneDockWidget::layerVisibilityChanged, this, [this](ObjectTopicId topic_id, bool visible) {
    list_->setRowVisible(topic_id.id, visible);
  });
  connect(dock, &SceneDockWidget::layerWarningChanged, this, [this](ObjectTopicId topic_id, bool warn, QString reason) {
    list_->setRowWarning(topic_id.id, warn, reason);
  });
}

void Scene2DConfigPanel::disconnectDock() {
  if (dock_ != nullptr) {
    disconnect(dock_.data(), nullptr, this, nullptr);
  }
  dock_ = nullptr;
}

void Scene2DConfigPanel::rebuildList() {
  std::vector<LayerRow> rows;
  if (dock_ != nullptr) {
    for (const auto& info : dock_->layers()) {
      rows.push_back(LayerRow{info.topic_id.id, info.display_name, info.visible});
    }
  }
  list_->setRows(rows);
  updateConfigPane();
}

void Scene2DConfigPanel::updateConfigPane() {
  config_host_->clear();
  if (dock_ == nullptr) {
    return;
  }
  const auto selected = list_->currentId();
  if (!selected.has_value()) {
    return;
  }
  ISceneLayer* layer = dock_->layerFor(toTopicId(*selected));
  if (layer == nullptr) {
    return;
  }
  if (QWidget* config = layer->createConfigWidget(config_host_)) {
    config_host_->setConfigWidget(config);
  }
}

void Scene2DConfigPanel::onStylesheetChanged(QString theme) {
  theme_ = std::move(theme);
  if (list_ != nullptr) {
    list_->setTheme(theme_);
  }
}

}  // namespace PJ
