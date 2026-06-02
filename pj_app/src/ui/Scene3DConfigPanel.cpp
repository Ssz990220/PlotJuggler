// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "ui/Scene3DConfigPanel.h"

#include <QAbstractItemView>
#include <QDropEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene3d_entity.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {
constexpr int kTopicIdRole = Qt::UserRole + 1;

// Mirror CurveEditor's row icon contract — same SVGs, same theme-aware
// LoadSvg path — so the 3D topic list reads as a peer of the plot's
// curve list rather than a one-off.
constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";

// Row height matches CurveEditor::kDefaultRowHeight; spacing matches
// kRowSpacing. Keeping the constants identical means the two lists
// produce visually identical row chrome.
constexpr int kDefaultRowHeight = 20;
constexpr int kRowSpacing = 2;

// Per-row widget pinned by absolute geometry (NOT a QHBoxLayout). The
// QListWidget viewport can shrink below the natural sum of the children's
// sizeHints — under a layout that would clip the row; under explicit
// geometry the ElidingLabel just elides further and the eye/trash icons
// stay anchored to the right edge. Same shape as CurveRowWidget.
class TopicRowWidget : public QWidget {
  Q_OBJECT
 public:
  TopicRowWidget(
      ObjectTopicId topic_id, const QString& display_name, bool visible, const QString& theme, int row_height,
      bool removable, QWidget* parent)
      : QWidget(parent),
        topic_id_(topic_id),
        current_theme_(theme),
        row_height_(row_height),
        removable_(removable),
        full_name_(display_name) {
    name_ = new ElidingLabel(this);
    // Left elide so the meaningful leaf (`/leaf_name`) stays visible as
    // the row narrows — the user explicitly asked for this and it
    // matches CurveEditor::appendRow's choice.
    name_->setElideMode(Qt::ElideLeft);
    name_->setFullText(display_name);
    name_->setToolTip(display_name);

    // Only the name label is made mouse-transparent — NOT the row container.
    // The row sits on its QListWidget item via setItemWidget; if the *container*
    // were transparent the list viewport would skip the whole item (eye/trash
    // included), killing those clicks. With just the label transparent, a press
    // on the name area falls through to the (opaque) container, which ignores it
    // by default so it propagates to the viewport and starts an InternalMove
    // drag-reorder; the eye/trash buttons (opaque children) stay clickable. The
    // hover tooltip moves to the QListWidgetItem since a transparent label gets
    // no hover.
    name_->setAttribute(Qt::WA_TransparentForMouseEvents);

    eye_ = new QToolButton(this);
    eye_->setObjectName(QStringLiteral("curveVisibilityToggle"));
    eye_->setCheckable(true);
    eye_->setChecked(visible);
    eye_->setAutoRaise(true);
    eye_->setFocusPolicy(Qt::NoFocus);
    eye_->setToolTip(tr("Toggle topic visibility"));

    // The TF display row is permanent, so it gets no trash button (removable
    // == false). Everything else (eye toggle, name, drag) is unchanged.
    if (removable_) {
      trash_ = new QToolButton(this);
      trash_->setObjectName(QStringLiteral("curveTrashToggle"));
      trash_->setAutoRaise(true);
      trash_->setFocusPolicy(Qt::NoFocus);
      trash_->setToolTip(tr("Remove this topic from the scene"));
    }

    refreshIcons();

    connect(eye_, &QToolButton::toggled, this, [this](bool checked) {
      eye_->setIcon(LoadSvg(checked ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
      emit visibilityToggled(topic_id_, checked);
    });
    if (trash_ != nullptr) {
      connect(trash_, &QToolButton::clicked, this, [this]() { emit removeClicked(topic_id_); });
    }
  }

  void setVisibleState(bool visible) {
    if (eye_->isChecked() == visible) {
      return;
    }
    QSignalBlocker block(eye_);
    eye_->setChecked(visible);
    eye_->setIcon(LoadSvg(visible ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
  }

  void setDisplayName(const QString& name) {
    full_name_ = name;
    name_->setFullText(name);
    name_->setToolTip(orphan_reason_.isEmpty() ? name : orphan_reason_);
  }

  // Orphan visuals: red ink on the name label + tooltip explaining why.
  // Updates the actual ElidingLabel widget directly because the row uses
  // setItemWidget(), so QListWidgetItem::setForeground/setToolTip don't
  // reach the visible label.
  void setOrphanState(bool is_orphan, const QString& reason) {
    if (is_orphan_ == is_orphan && orphan_reason_ == reason) {
      return;
    }
    is_orphan_ = is_orphan;
    orphan_reason_ = reason;
    if (is_orphan_) {
      name_->setStyleSheet(QStringLiteral("color: #d32f2f;"));
      name_->setToolTip(orphan_reason_);
    } else {
      name_->setStyleSheet(QString{});
      name_->setToolTip(full_name_);
    }
  }

  void setTheme(const QString& theme) {
    current_theme_ = theme;
    refreshIcons();
  }

  void setRowHeight(int row_height) {
    if (row_height == row_height_) {
      return;
    }
    row_height_ = row_height;
    refreshIcons();
    updateGeometry();
  }

  [[nodiscard]] QSize sizeHint() const override {
    return {0, row_height_};
  }

 signals:
  void visibilityToggled(ObjectTopicId topic_id, bool visible);
  void removeClicked(ObjectTopicId topic_id);

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    const int h = height();
    const int total_w = width();

    // With a trash button the eye sits to its left; without one (TF row) the
    // eye takes the rightmost slot.
    int eye_x = total_w - h;
    if (trash_ != nullptr) {
      const int trash_x = total_w - h;
      trash_->setGeometry(trash_x, 0, h, h);
      eye_x = trash_x - kRowSpacing - h;
    }
    eye_->setGeometry(eye_x, 0, h, h);

    // Name claims the full width from the left edge up to the eye button,
    // since there's no family marker on the left edge any more.
    const int name_x = kRowSpacing;
    const int name_right = eye_x - kRowSpacing;
    const int name_width = name_right - name_x;
    if (name_width < 1) {
      if (name_->isVisible()) {
        name_->setVisible(false);
      }
      return;
    }
    if (!name_->isVisible()) {
      name_->setVisible(true);
    }
    name_->setGeometry(name_x, 0, name_width, h);
  }

 private:
  void refreshIcons() {
    const QSize sz(row_height_, row_height_);
    eye_->setIconSize(sz);
    eye_->setIcon(LoadSvg(eye_->isChecked() ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
    if (trash_ != nullptr) {
      trash_->setIconSize(sz);
      trash_->setIcon(LoadSvg(kTrashIconPath, current_theme_));
    }
  }

  ObjectTopicId topic_id_;
  QString current_theme_;
  int row_height_;
  bool removable_ = true;
  QString full_name_;
  QString orphan_reason_;
  bool is_orphan_ = false;
  ElidingLabel* name_ = nullptr;
  QToolButton* eye_ = nullptr;
  QToolButton* trash_ = nullptr;
};

// QListWidget that supports drag-reorder of rows that carry setItemWidget
// widgets. It keeps the built-in InternalMove drag visuals but overrides the
// drop to merely REPORT the move (from→to) instead of letting QListWidget
// remove+reinsert the item — which would delete the row's custom widget and can
// duplicate items. The panel rebuilds the rows from the new order in response.
class TopicListWidget : public QListWidget {
  Q_OBJECT
 public:
  explicit TopicListWidget(QWidget* parent) : QListWidget(parent) {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
  }

 signals:
  void rowMoved(int from, int to);

 protected:
  void dropEvent(QDropEvent* event) override {
    if (event->source() != this) {
      event->ignore();
      return;
    }
    const int from = currentRow();
    int to = count();  // dropped past the last row → end
    if (const QModelIndex idx = indexAt(event->position().toPoint()); idx.isValid()) {
      to = idx.row();
      if (dropIndicatorPosition() == QAbstractItemView::BelowItem) {
        ++to;
      }
    }
    // Accept with IgnoreAction (NOT the proposed MoveAction): a MoveAction would
    // make QAbstractItemView::startDrag run clearOrRemove() after this returns,
    // deleting the dragged source row — which, since the panel rebuilds the list
    // itself, would drop an item. IgnoreAction leaves the model untouched; the
    // panel's (queued) rebuild is the sole mutation.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    if (from >= 0) {
      emit rowMoved(from, to);
    }
    // Deliberately NOT calling QListWidget::dropEvent — the panel rebuilds rows
    // from the reported order, avoiding item recreation / widget loss.
  }
};

}  // namespace

Scene3DConfigPanel::Scene3DConfigPanel(QWidget* parent) : QWidget(parent), current_theme_(PJ::currentTheme()) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(8, 8, 8, 8);
  root->setSpacing(8);

  // --- Topic list ---
  // Wrapped in a framed QGroupBox-style container so the list visually
  // reads as one unit. Fixed-frame selection lives in the 3D viewport as a
  // floating combo (owned by Scene3DDockWidget) — this panel is topics-only.
  auto* topics_frame = new QFrame(this);
  topics_frame->setFrameShape(QFrame::StyledPanel);
  topics_frame->setFrameShadow(QFrame::Sunken);
  auto* topics_frame_layout = new QVBoxLayout(topics_frame);
  topics_frame_layout->setContentsMargins(6, 6, 6, 6);
  topics_frame_layout->setSpacing(4);

  auto* topics_label = new QLabel(tr("Topics"), topics_frame);
  topics_label->setStyleSheet(QStringLiteral("font-weight: bold;"));
  topics_frame_layout->addWidget(topics_label);

  auto* topics_list = new TopicListWidget(topics_frame);
  topics_list_ = topics_list;
  topics_list_->setUniformItemSizes(true);
  topics_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  topics_list_->setResizeMode(QListView::Adjust);
  topics_list_->setSpacing(0);
  topics_list_->setMinimumWidth(0);
  topics_list_->setStyleSheet(QStringLiteral("QListWidget::item { padding: 0px; }"));
  // Reasonably small default — ~3 rows visible — and scrolls internally
  // when more topics are attached. The params section sits immediately
  // below so users see entity controls without scrolling the panel.
  topics_list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  topics_list_->setMinimumHeight(kDefaultRowHeight * 2);
  topics_list_->setMaximumHeight(kDefaultRowHeight * 6 + 4);
  topics_frame_layout->addWidget(topics_list_);

  root->addWidget(topics_frame);

  // Horizontal rule between the framed topics block and the params form.
  auto* sep_rule = new QFrame(this);
  sep_rule->setFrameShape(QFrame::HLine);
  sep_rule->setFrameShadow(QFrame::Sunken);
  root->addWidget(sep_rule);

  // --- Selected entity params ---
  // Container repopulated on each topic-list selection with whatever
  // Scene3DEntity::createConfigWidget returns. The panel never inspects
  // the contents of that widget.
  params_container_ = new QWidget(this);
  params_layout_ = new QVBoxLayout(params_container_);
  params_layout_->setContentsMargins(0, 0, 0, 0);
  params_layout_->setSpacing(4);
  root->addWidget(params_container_);

  // Push the (compact) topics frame + params block to the top of the panel.
  root->addStretch(1);

  connect(topics_list_, &QListWidget::itemSelectionChanged, this, &Scene3DConfigPanel::onTopicSelectionChanged);
  // Queued: run the rebuild after the drop event (and the base view's drag
  // teardown) has fully unwound, so we never clear the list mid-drop.
  connect(topics_list, &TopicListWidget::rowMoved, this, &Scene3DConfigPanel::onRowMoved, Qt::QueuedConnection);

  updateSelectedEntityPane();
}

void Scene3DConfigPanel::bindDock(Scene3DDockWidget* dock) {
  if (bound_dock_.data() == dock) {
    return;
  }
  disconnectFromDock();
  bound_dock_ = dock;

  topics_list_->clear();
  topic_items_.clear();
  clearParamsContainer();

  if (dock == nullptr) {
    updateSelectedEntityPane();
    return;
  }

  rebuildTopicList();

  connect(dock, &Scene3DDockWidget::entityAdded, this, &Scene3DConfigPanel::onDockEntityAdded);
  connect(dock, &Scene3DDockWidget::entityRemoved, this, &Scene3DConfigPanel::onDockEntityRemoved);
  connect(dock, &Scene3DDockWidget::entityVisibilityChanged, this, &Scene3DConfigPanel::onDockEntityVisibilityChanged);
  connect(dock, &Scene3DDockWidget::entityOrphanChanged, this, &Scene3DConfigPanel::onDockEntityOrphanChanged);
  // TF binds with no entityAdded (e.g. a /tf-only drop), so rebuild the list
  // when it announces presence to surface the permanent TF row.
  connect(dock, &Scene3DDockWidget::tfPresenceChanged, this, [this](bool) { rebuildTopicList(); });
}

void Scene3DConfigPanel::disconnectFromDock() {
  if (bound_dock_ != nullptr) {
    disconnect(bound_dock_.data(), nullptr, this, nullptr);
  }
  bound_dock_ = nullptr;
}

void Scene3DConfigPanel::rebuildTopicList() {
  topics_list_->clear();
  topic_items_.clear();
  if (bound_dock_ == nullptr) {
    updateSelectedEntityPane();
    return;
  }
  for (const auto& info : bound_dock_->entities()) {
    auto* item = new QListWidgetItem(topics_list_);
    installRowWidget(item, info.topic_id, info.display_name, info.visible);
  }
  appendTfRow();
  // Auto-select the first topic so the user sees a populated params
  // pane immediately on first drop.
  if (topics_list_->count() > 0) {
    topics_list_->setCurrentRow(0);
  } else {
    updateSelectedEntityPane();
  }
}

void Scene3DConfigPanel::appendTfRow() {
  if (bound_dock_ == nullptr || !bound_dock_->tfPresent()) {
    return;
  }
  // TF is a permanent dataset-wide display (the axis triads), not a droppable
  // topic — a fixed, non-removable row with just a name + visibility eye,
  // pinned below the entity rows. Its eye drives the dock's TF visibility.
  auto* item = new QListWidgetItem(topics_list_);
  auto* row = new TopicRowWidget(
      ObjectTopicId{0}, QStringLiteral("/tf"), bound_dock_->tfVisible(), current_theme_, kDefaultRowHeight,
      /*removable=*/false, topics_list_);
  item->setSizeHint(QSize(0, kDefaultRowHeight));
  item->setToolTip(tr("Transform frames (TF) — always present"));
  // Not drag-reorderable: it's pinned and carries no topic id, so it must not
  // pollute the entity drag-reorder (onRowMoved reads kTopicIdRole).
  item->setFlags(item->flags() & ~Qt::ItemIsDragEnabled);
  topics_list_->setItemWidget(item, row);
  connect(row, &TopicRowWidget::visibilityToggled, this, [this](ObjectTopicId, bool v) {
    if (bound_dock_ != nullptr) {
      bound_dock_->setTfVisible(v);
    }
  });
}

void Scene3DConfigPanel::installRowWidget(
    QListWidgetItem* item, ObjectTopicId topic_id, const QString& name, bool visible) {
  auto* row =
      new TopicRowWidget(topic_id, name, visible, current_theme_, kDefaultRowHeight, /*removable=*/true, topics_list_);
  // Fixed height per row so QListWidget pre-computes positions identically
  // to CurveEditor; width comes from the viewport (0 stretches).
  item->setSizeHint(QSize(0, kDefaultRowHeight));
  item->setData(kTopicIdRole, QVariant::fromValue(static_cast<qulonglong>(topic_id.id)));
  // The view shows this on hover (the row's name label is mouse-transparent for
  // drag, so it can't host the tooltip itself). onDockEntityOrphanChanged swaps
  // it for the orphan reason while orphaned.
  item->setToolTip(name);
  topics_list_->setItemWidget(item, row);
  topic_items_[topic_id.id] = item;

  // Replay any orphan state the dock already has for this entity. Needed
  // when binding to an already-populated dock — entityOrphanChanged only
  // fires on transitions, not on initial state.
  if (bound_dock_ != nullptr) {
    const auto snap = bound_dock_->orphanState(topic_id);
    if (snap.is_orphan) {
      row->setOrphanState(true, snap.reason);
    }
  }

  connect(row, &TopicRowWidget::visibilityToggled, this, [this](ObjectTopicId tid, bool v) {
    if (bound_dock_ != nullptr) {
      bound_dock_->setTopicVisible(tid, v);
    }
  });
  connect(row, &TopicRowWidget::removeClicked, this, [this](ObjectTopicId tid) {
    if (bound_dock_ != nullptr) {
      bound_dock_->removeTopic(tid);
    }
  });
}

std::optional<ObjectTopicId> Scene3DConfigPanel::selectedTopicId() const {
  auto* item = topics_list_->currentItem();
  if (item == nullptr) {
    return std::nullopt;
  }
  ObjectTopicId id;
  id.id = static_cast<int64_t>(item->data(kTopicIdRole).toULongLong());
  return id;
}

void Scene3DConfigPanel::updateSelectedEntityPane() {
  clearParamsContainer();
  const auto sel = selectedTopicId();
  if (!sel.has_value() || bound_dock_ == nullptr) {
    return;
  }
  pj::scene3d::Scene3DEntity* entity = bound_dock_->entityFor(*sel);
  if (entity == nullptr) {
    return;
  }

  // Ask the entity for a fresh config widget. Whatever it returns goes
  // into the container as-is — the panel does not introspect it. The
  // returned widget is parented to the container, so it's destroyed
  // automatically the next time clearParamsContainer() runs.
  if (QWidget* config = entity->createConfigWidget(params_container_)) {
    params_layout_->addWidget(config);
  }
}

void Scene3DConfigPanel::clearParamsContainer() {
  if (params_layout_ == nullptr) {
    return;
  }
  while (QLayoutItem* item = params_layout_->takeAt(0)) {
    if (QWidget* w = item->widget()) {
      w->deleteLater();
    }
    delete item;
  }
}

void Scene3DConfigPanel::onTopicSelectionChanged() {
  updateSelectedEntityPane();
}

void Scene3DConfigPanel::onRowMoved(int from, int to) {
  // Snapshot the current row order (topic ids carried in kTopicIdRole).
  std::vector<int64_t> ids;
  ids.reserve(static_cast<std::size_t>(topics_list_->count()));
  for (int i = 0; i < topics_list_->count(); ++i) {
    ids.push_back(static_cast<int64_t>(topics_list_->item(i)->data(kTopicIdRole).toULongLong()));
  }
  if (from < 0 || from >= static_cast<int>(ids.size())) {
    return;
  }
  // Move `from` to `to`; removing `from` shifts later targets down by one.
  int dst = std::clamp(to, 0, static_cast<int>(ids.size()));
  const int64_t moved = ids[static_cast<std::size_t>(from)];
  ids.erase(ids.begin() + from);
  if (dst > from) {
    --dst;
  }
  dst = std::clamp(dst, 0, static_cast<int>(ids.size()));
  ids.insert(ids.begin() + dst, moved);

  rebuildFromOrder(ids, moved);

  // Push the new order down to the dock → SceneViewWidget render/draw order
  // (index 0 drawn first / behind; last drawn on top).
  std::vector<ObjectTopicId> ordered;
  ordered.reserve(ids.size());
  for (const int64_t id : ids) {
    ObjectTopicId tid;
    tid.id = id;
    ordered.push_back(tid);
  }
  if (bound_dock_ != nullptr) {
    bound_dock_->reorderEntities(ordered);
  }
}

void Scene3DConfigPanel::rebuildFromOrder(const std::vector<int64_t>& ordered_ids, int64_t select_id) {
  topic_items_.clear();
  QListWidgetItem* to_select = nullptr;
  {
    // clear() drops selection (and the moved item's recreated widget); suppress
    // the churn, then re-add each row in the new order via installRowWidget.
    const QSignalBlocker block(topics_list_);
    topics_list_->clear();
    for (const int64_t id : ordered_ids) {
      ObjectTopicId tid;
      tid.id = id;
      pj::scene3d::Scene3DEntity* entity = (bound_dock_ != nullptr) ? bound_dock_->entityFor(tid) : nullptr;
      if (entity == nullptr) {
        continue;
      }
      const auto info = entity->info();
      auto* item = new QListWidgetItem(topics_list_);
      installRowWidget(item, tid, info.display_name, info.visible);
      if (id == select_id) {
        to_select = item;
      }
    }
    // Re-pin the permanent TF row below the reordered entities — it carries no
    // topic id, so it isn't part of ordered_ids and would otherwise be lost.
    appendTfRow();
  }
  if (to_select != nullptr) {
    topics_list_->setCurrentItem(to_select);  // one selection signal → refresh params pane
  } else {
    updateSelectedEntityPane();
  }
}

void Scene3DConfigPanel::onStylesheetChanged(QString theme) {
  current_theme_ = std::move(theme);
  // Re-tint every row's eye + trash glyphs through the active theme ink.
  for (int i = 0; i < topics_list_->count(); ++i) {
    QWidget* row = topics_list_->itemWidget(topics_list_->item(i));
    if (auto* trw = qobject_cast<TopicRowWidget*>(row)) {
      trw->setTheme(current_theme_);
    }
  }
}

void Scene3DConfigPanel::onDockEntityAdded(ObjectTopicId topic_id) {
  if (topic_items_.contains(topic_id.id)) {
    return;
  }
  if (bound_dock_ == nullptr) {
    return;
  }
  // Pull display name + visibility from the entity itself so the panel
  // doesn't carry per-kind knowledge in the signal payload.
  pj::scene3d::Scene3DEntity* entity = bound_dock_->entityFor(topic_id);
  if (entity == nullptr) {
    return;
  }
  const auto info = entity->info();
  auto* item = new QListWidgetItem(topics_list_);
  installRowWidget(item, topic_id, info.display_name, info.visible);
  // Auto-select the new entry if nothing was selected so the params
  // pane populates without the user having to click.
  if (topics_list_->currentItem() == nullptr) {
    topics_list_->setCurrentItem(item);
  }
}

void Scene3DConfigPanel::onDockEntityRemoved(ObjectTopicId topic_id) {
  auto it = topic_items_.find(topic_id.id);
  if (it == topic_items_.end()) {
    return;
  }
  // takeItem removes the item from the list and transfers ownership;
  // deleting it also destroys the row widget that was set via setItemWidget.
  const int row = topics_list_->row(it->second);
  delete topics_list_->takeItem(row);
  topic_items_.erase(it);
  updateSelectedEntityPane();
}

void Scene3DConfigPanel::onDockEntityVisibilityChanged(ObjectTopicId topic_id, bool visible) {
  auto it = topic_items_.find(topic_id.id);
  if (it == topic_items_.end()) {
    return;
  }
  if (auto* row = qobject_cast<TopicRowWidget*>(topics_list_->itemWidget(it->second))) {
    row->setVisibleState(visible);
  }
}

void Scene3DConfigPanel::onDockEntityOrphanChanged(ObjectTopicId topic_id, bool is_orphan, const QString& reason) {
  auto it = topic_items_.find(topic_id.id);
  if (it == topic_items_.end()) {
    return;
  }
  if (auto* row = qobject_cast<TopicRowWidget*>(topics_list_->itemWidget(it->second))) {
    row->setOrphanState(is_orphan, reason);
  }
  // Mirror the reason onto the item tooltip (the visible label is mouse-transparent
  // for drag, so the view supplies the hover tooltip). Restore the name when healed.
  if (is_orphan && !reason.isEmpty()) {
    it->second->setToolTip(reason);
  } else if (bound_dock_ != nullptr) {
    if (auto* entity = bound_dock_->entityFor(topic_id)) {
      it->second->setToolTip(entity->info().display_name);
    }
  }
}

}  // namespace PJ

#include "Scene3DConfigPanel.moc"
