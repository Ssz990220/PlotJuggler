#include "ui/CurveTreeView.h"

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>

#include <algorithm>

namespace PJ {

namespace {
constexpr int kNameColumn = 0;
constexpr int kValueColumn = 1;

QStringList splitPath(const QString& name) {
  return name.split('/', Qt::SkipEmptyParts);
}
}  // namespace

CurveTreeView::CurveTreeView(QWidget* parent) : QTreeWidget(parent) {
  setColumnCount(2);
  setHeaderLabels({tr("Name"), tr("Value")});
  header()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
  header()->setSectionResizeMode(kValueColumn, QHeaderView::ResizeToContents);
  header()->setSectionsClickable(false);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setRootIsDecorated(true);
  setUniformRowHeights(true);
  // Drag is handled manually in mouseMoveEvent — PJ3 did the same because it
  // needs two different mime types depending on mouse button.
  setDragEnabled(false);
  setDragDropMode(QAbstractItemView::NoDragDrop);
}

QTreeWidgetItem* CurveTreeView::ensureGroup(const QString& path) {
  const QStringList parts = splitPath(path);
  QTreeWidgetItem* parent = invisibleRootItem();
  for (const QString& part : parts) {
    QTreeWidgetItem* found = nullptr;
    for (int i = 0; i < parent->childCount(); ++i) {
      auto* child = parent->child(i);
      if (child->text(kNameColumn) == part) {
        found = child;
        break;
      }
    }
    if (!found) {
      found = new QTreeWidgetItem(parent);
      found->setText(kNameColumn, part);
      found->setFlags(found->flags() & ~Qt::ItemIsDragEnabled);
    }
    parent = found;
  }
  return parent;
}

void CurveTreeView::addCurve(const QString& name) {
  const int last_sep = name.lastIndexOf('/');
  QTreeWidgetItem* parent = invisibleRootItem();
  QString leaf_name = name;
  if (last_sep >= 0) {
    parent = ensureGroup(name.left(last_sep));
    leaf_name = name.mid(last_sep + 1);
  }
  auto* item = new QTreeWidgetItem(parent);
  item->setText(kNameColumn, leaf_name);
  item->setData(kNameColumn, Qt::UserRole, name);
  item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
}

void CurveTreeView::clearCurves() {
  clear();
}

void CurveTreeView::applyFilter(const QString& filter) {
  const QStringList tokens = filter.split(' ', Qt::SkipEmptyParts);

  // Recursive show-hide: an item is visible if any descendant matches all
  // tokens (case-insensitive substring, AND).
  std::function<bool(QTreeWidgetItem*)> apply = [&](QTreeWidgetItem* item) {
    bool any_child_visible = false;
    for (int i = 0; i < item->childCount(); ++i) {
      any_child_visible = apply(item->child(i)) || any_child_visible;
    }
    const QString full = item->data(kNameColumn, Qt::UserRole).toString();
    const QString haystack = full.isEmpty() ? item->text(kNameColumn) : full;
    const bool self_match = std::all_of(tokens.begin(), tokens.end(), [&](const QString& token) {
      return haystack.contains(token, Qt::CaseInsensitive);
    });
    const bool visible = any_child_visible || self_match;
    item->setHidden(!visible);
    return visible;
  };

  for (int i = 0; i < topLevelItemCount(); ++i) {
    apply(topLevelItem(i));
  }
}

std::vector<QString> CurveTreeView::selectedCurveNames() const {
  std::vector<QString> names;
  for (auto* item : selectedItems()) {
    const QString full = item->data(kNameColumn, Qt::UserRole).toString();
    if (!full.isEmpty()) {
      names.push_back(full);
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

void CurveTreeView::setValuesColumnHidden(bool hidden) {
  setColumnHidden(kValueColumn, hidden);
}

void CurveTreeView::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
    drag_start_pos_ = event->pos();
    drag_button_ = event->button();
  }
  QTreeWidget::mousePressEvent(event);
}

void CurveTreeView::mouseMoveEvent(QMouseEvent* event) {
  if (drag_button_ == Qt::NoButton) {
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if ((event->pos() - drag_start_pos_).manhattanLength() < QApplication::startDragDistance()) {
    QTreeWidget::mouseMoveEvent(event);
    return;
  }

  auto names = selectedCurveNames();
  if (names.empty()) {
    drag_button_ = Qt::NoButton;
    QTreeWidget::mouseMoveEvent(event);
    return;
  }

  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& name : names) {
    stream << name;
  }

  auto* mime_data = new QMimeData();
  // PJ3 contract: left-button drag → add curve; right-button drag with
  // exactly two curves → XY scatter plot. We preserve both mime keys.
  if (drag_button_ == Qt::LeftButton) {
    mime_data->setData("curveslist/add_curve", encoded);
  } else if (drag_button_ == Qt::RightButton && names.size() == 2) {
    mime_data->setData("curveslist/new_XY_axis", encoded);
  } else {
    delete mime_data;
    drag_button_ = Qt::NoButton;
    return;
  }

  auto* drag = new QDrag(this);
  drag->setMimeData(mime_data);
  drag_button_ = Qt::NoButton;
  drag->exec(Qt::CopyAction | Qt::MoveAction);
}

}  // namespace PJ
