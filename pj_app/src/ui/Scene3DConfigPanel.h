#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QString>
#include <QWidget>
#include <optional>
#include <vector>

#include "pj_datastore/object_store.hpp"

namespace PJ {

class ConfigPanelHost;
class LayerListView;
class Scene3DDockWidget;

class Scene3DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene3DConfigPanel(QWidget* parent = nullptr);
  ~Scene3DConfigPanel() override = default;

  void bindDock(Scene3DDockWidget* dock);

 public slots:
  void onStylesheetChanged(QString theme);

 private slots:
  void onLayerSelectionChanged();
  void onLayerAdded(ObjectTopicId topic_id);
  void onLayerRemoved(ObjectTopicId topic_id);
  void onLayerVisibilityChanged(ObjectTopicId topic_id, bool visible);
  void onLayerWarningChanged(ObjectTopicId topic_id, bool warn, const QString& reason);

 private:
  void disconnectFromDock();
  void rebuildLayerList();
  void updateSelectedLayerPane();
  [[nodiscard]] std::optional<ObjectTopicId> selectedTopicId() const;
  [[nodiscard]] std::vector<ObjectTopicId> topicOrderFromIds(const std::vector<qint64>& ids) const;

  LayerListView* layer_list_ = nullptr;
  ConfigPanelHost* config_host_ = nullptr;
  QPointer<Scene3DDockWidget> bound_dock_;
};

}  // namespace PJ
