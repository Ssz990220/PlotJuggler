#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QPointer>
#include <QWidget>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pj_datastore/object_store.hpp"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QVBoxLayout;

namespace PJ {

class Scene3DDockWidget;

// Right-sidepanel content for the active Scene3DDockWidget.
//
// Layout:
//   - Topic list. One row per attached entity; each row shows an eye
//     toggle (visibility), the topic name, and a trash button (remove).
//   - Selected-entity params. The panel asks the selected entity for a
//     fresh config widget (Scene3DEntity::createConfigWidget) and
//     inserts it into a container — the panel is type-blind. PointCloud
//     entities return a color-field combo; future URDF entities will
//     return visual/collision toggles; etc.
//
// Fixed-frame selection lives inside the 3D viewport as a floating combo
// owned by Scene3DDockWidget — not this panel. The dock owns the truth
// (which topic is attached, which fixed frame). Per-instance parameters
// are owned by each entity. The panel is a thin reactive UI that observes
// dock signals and translates user input into dock / entity slot calls.
class Scene3DConfigPanel : public QWidget {
  Q_OBJECT
 public:
  explicit Scene3DConfigPanel(QWidget* parent = nullptr);
  ~Scene3DConfigPanel() override = default;

  // Attach the panel to a dock. Pass nullptr to detach. Safe to call
  // repeatedly with the same dock.
  void bindDock(Scene3DDockWidget* dock);

 public slots:
  // Re-tint per-row eye / trash icons through the new theme palette.
  // Mirrors CurveEditor::onStylesheetChanged so MainWindow can wire both
  // panels through its existing stylesheetChanged signal.
  void onStylesheetChanged(QString theme);

 private slots:
  void onTopicSelectionChanged();
  // Drag-reorder: the list reports a row move (from→to); we recompute the topic
  // order, rebuild the rows in that order, and push it to the dock so it drives
  // the entity render/draw order (top of list = drawn first / behind).
  void onRowMoved(int from, int to);

  // Dock events → panel.
  void onDockEntityAdded(ObjectTopicId topic_id);
  void onDockEntityRemoved(ObjectTopicId topic_id);
  void onDockEntityVisibilityChanged(ObjectTopicId topic_id, bool visible);
  void onDockEntityOrphanChanged(ObjectTopicId topic_id, bool is_orphan, const QString& reason);

 private:
  void disconnectFromDock();
  void rebuildTopicList();
  void updateSelectedEntityPane();
  // Tears down whatever config widget was last installed in the params
  // container so the next selection starts from a clean slate.
  void clearParamsContainer();
  // Returns the topic_id of the currently selected list row, or {} if none.
  std::optional<ObjectTopicId> selectedTopicId() const;
  // Create the per-row widget (eye toggle + name label + trash button) for
  // a topic and attach it to the given list item.
  void installRowWidget(QListWidgetItem* item, ObjectTopicId topic_id, const QString& name, bool visible);
  // Append the permanent, non-removable TF display row (name + visibility eye,
  // no trash) when the bound dock has TF. Called from every list rebuild so TF
  // survives drag-reorders. No-op when the dock has no TF.
  void appendTfRow();
  // Clear and re-add the topic rows in `ordered_ids`, re-selecting `select_id`.
  // Used after a drag-reorder so the rows (with their custom widgets) are rebuilt
  // deterministically rather than relying on QListWidget's item-move handling.
  void rebuildFromOrder(const std::vector<int64_t>& ordered_ids, int64_t select_id);

  QListWidget* topics_list_ = nullptr;
  // Holds whatever QWidget the selected entity's createConfigWidget()
  // returned. Replaced on every selection change.
  QWidget* params_container_ = nullptr;
  QVBoxLayout* params_layout_ = nullptr;
  QPointer<Scene3DDockWidget> bound_dock_;
  QString current_theme_;

  // topic_id.id → QListWidgetItem* for O(1) lookup on remove/visibility
  // updates. The item's Qt::UserRole carries the topic_id.id as well so
  // we can recover it from selection signals.
  std::unordered_map<int64_t, QListWidgetItem*> topic_items_;
};

}  // namespace PJ
