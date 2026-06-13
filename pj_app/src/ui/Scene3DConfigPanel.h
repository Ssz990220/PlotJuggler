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
#include "pj_widgets/SettingsDebouncer.h"

class QComboBox;
class QEvent;
class QToolButton;
class QVBoxLayout;

namespace PJ {

class ConfigPanelHost;
class DoubleScrubber;
class IntScrubber;
class LayerListView;
class Scene3DDockWidget;

// QSettings group for the scene-wide controls. Single source for both the
// read side (buildSceneControls) and the write side (settings_writer_).
inline constexpr char kScene3dSceneControlsGroup[] = "pj_scene3d/scene_controls";

// Right-sidepanel page for the 3D scene: scene-wide controls (Grid · Transforms
// and RobotModel — plan §9 Part C) above the per-topic layer list. Scene
// controls persist in QSettings (pj_scene3d/scene_controls/*) and are applied
// to every dock this panel binds, so all 3D views share one look. The
// Model/URDF row adds robot-model layers to the bound dock (File dialog /
// robot_description topic picker / URL prompt, chosen by the source combo).
//
// Robot-model rows are derived from dock state, not from the act of adding:
// EVERY kRobotDescription layer on the bound dock gets exactly one row, rebuilt
// in rebuildLayerList()/onLayerAdded() on bind, dock switch, and layout restore
// (the rows belong to the dock, so a rebind clears and rebuilds them). Robot
// layers stay OUT of the Topics list; their row is their whole list-side UI.
// Clicking a row binds the Settings host to that layer's config widget, so the
// source combo / status text / Retry are reachable.
class Scene3DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene3DConfigPanel(QWidget* parent = nullptr);

  void bindDock(Scene3DDockWidget* dock);

  // Routes a left-click on a robot row's name field to showRobotLayerConfig.
  // The row's topic id rides on the watched widget's "robot_topic_id" property.
  bool eventFilter(QObject* watched, QEvent* event) override;

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

 public:
  // Push every persisted scene-control value into an arbitrary dock's view
  // (not just bound_dock_). Used to bring a dock created by layout restore up to
  // the shared look the moment its lazily-created view exists, even if the panel
  // never bound it (M.3). No-op when the dock's view is not yet realized.
  void applySceneControlsTo(Scene3DDockWidget* dock);

 private:
  void applyIcons();
  // "+" button: add a robot model via the mode picked in the source combo —
  // File opens a file dialog (last dir remembered), Topic a dialog listing the
  // dataset's robot_description topics, URL a dialog with a line edit.
  void onAddModelClicked();
  // Append/remove the per-robot row (name + bin button) for a kRobotDescription
  // layer on the bound dock. Idempotent: a no-op if a row for topic_id_value
  // already exists, so repeated rebuilds and the add-then-layerAdded double-fire
  // both collapse to one row. The row is clickable — it binds config_host_ to
  // the layer's config widget so its source combo / status / Retry are reachable.
  void addRobotRow(uint32_t topic_id_value, const QString& label, const QString& tooltip);
  void removeRobotRowFor(uint32_t topic_id_value);
  // Bind the Settings host to the config widget of the kRobotDescription layer
  // for topic_id_value, when one exists on the bound dock. Called from a row click.
  void showRobotLayerConfig(uint32_t topic_id_value);
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

  // Debounced QSettings persistence: scene-control changes apply live every tick
  // but only flush to disk after the drag settles (one INI rewrite per drag).
  SettingsDebouncer settings_writer_{QString::fromLatin1(kScene3dSceneControlsGroup), 250};
};

}  // namespace PJ
