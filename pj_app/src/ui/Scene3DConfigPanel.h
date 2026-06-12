#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "pj_datastore/object_store.hpp"

class QComboBox;
class QToolButton;
class QVBoxLayout;

namespace PJ {

class ConfigPanelHost;
class DoubleScrubber;
class IntScrubber;
class LayerListView;
class Scene3DDockWidget;

// Right-sidepanel page for the 3D scene: scene-wide controls (Grid · Transforms
// and RobotModel — plan §9 Part C) above the per-topic layer list. Scene
// controls persist in QSettings (pj_scene3d/scene_controls/*) and are applied
// to every dock this panel binds, so all 3D views share one look. The
// Model/URDF row adds robot-model layers to the bound dock (File dialog /
// robot_description topic picker / URL prompt, chosen by the source combo);
// each added robot gets a row below with a bin button to remove it. Layers
// added by drag-and-drop are never touched and don't get a row.
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
  void buildSceneControls(QVBoxLayout* root);
  // Push every persisted scene-control value into the bound dock's view.
  void applySceneControls();
  void applyIcons();
  // "+" button: add a robot model via the mode picked in the source combo —
  // File opens a file dialog (last dir remembered), Topic a dialog listing the
  // dataset's robot_description topics, URL a dialog with a line edit.
  void onAddModelClicked();
  // Append/remove the per-robot row (name + bin button) for a layer this
  // panel created. Rows are panel-created-only: drag-and-drop robot layers
  // don't get one.
  void addRobotRow(uint32_t topic_id_value, const QString& label, const QString& tooltip);
  void removeRobotRowFor(uint32_t topic_id_value);
  void disconnectFromDock();
  void rebuildLayerList();
  void updateSelectedLayerPane();
  [[nodiscard]] std::optional<ObjectTopicId> selectedTopicId() const;
  [[nodiscard]] std::vector<ObjectTopicId> topicOrderFromIds(const std::vector<qint64>& ids) const;

  LayerListView* layer_list_ = nullptr;
  ConfigPanelHost* config_host_ = nullptr;
  QPointer<Scene3DDockWidget> bound_dock_;
  QString theme_{QStringLiteral("light")};

  // Scene controls (values mirrored in QSettings). The eye buttons are
  // checkable show/hide toggles, independent of the opacity values so hiding
  // and re-showing a feature keeps its tuned opacity.
  QToolButton* grid_lines_button_ = nullptr;
  QToolButton* grid_cells_button_ = nullptr;
  QToolButton* grid_eye_ = nullptr;
  DoubleScrubber* grid_size_ = nullptr;
  IntScrubber* grid_divisions_ = nullptr;
  DoubleScrubber* gizmo_size_ = nullptr;
  DoubleScrubber* gizmo_opacity_ = nullptr;
  QToolButton* gizmo_eye_ = nullptr;
  DoubleScrubber* mesh_opacity_ = nullptr;
  QToolButton* mesh_eye_ = nullptr;
  DoubleScrubber* collision_opacity_ = nullptr;
  QToolButton* collision_eye_ = nullptr;

  // Model/URDF row. Robot layers are panel-managed: they do not appear in the
  // Topics list, so the source combo + add button + per-robot rows below are
  // their whole UI. robot_rows_ maps each panel-created layer id to its row
  // widget; cleared on rebind (rows belong to the dock they were added to).
  QComboBox* model_source_combo_ = nullptr;
  QToolButton* add_model_button_ = nullptr;
  QVBoxLayout* robot_rows_layout_ = nullptr;
  std::vector<std::pair<uint32_t, QWidget*>> robot_rows_;
};

}  // namespace PJ
