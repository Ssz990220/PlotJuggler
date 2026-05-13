#include "pj_widgets/CurveTreeView.h"

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QHeaderView>
#include <QMimeData>
#include <QMouseEvent>
#include <algorithm>
#include <functional>
#include <utility>

namespace PJ {

namespace {
constexpr int kNameColumn = 0;
constexpr int kValueColumn = 1;

QStringList splitPath(const QString& name) {
  return name.split('/', Qt::SkipEmptyParts);
}

class CurveTreeItem : public QTreeWidgetItem {
 public:
  explicit CurveTreeItem(QTreeWidgetItem* parent) : QTreeWidgetItem(parent) {}

  bool operator<(const QTreeWidgetItem& other) const override {
    const QString lhs = text(kNameColumn);
    const QString rhs = other.text(kNameColumn);
    const int folded_compare = QString::localeAwareCompare(lhs.toCaseFolded(), rhs.toCaseFolded());
    if (folded_compare != 0) {
      return folded_compare < 0;
    }
    return QString::localeAwareCompare(lhs, rhs) < 0;
  }
};

void normalizeCurveNames(std::vector<QString>& names) {
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
}
}  // namespace

CurveTreeView::CurveTreeView(QWidget* parent) : QTreeWidget(parent) {
  setColumnCount(2);
  setHeaderLabels({tr("Name"), tr("Value")});
  header()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
  header()->setSectionResizeMode(kValueColumn, QHeaderView::ResizeToContents);
  header()->setSectionsClickable(false);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setSelectionBehavior(QAbstractItemView::SelectRows);
  setFocusPolicy(Qt::ClickFocus);
  setRootIsDecorated(true);
  setUniformRowHeights(true);
  setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
  setExpandsOnDoubleClick(false);
  // Drag handled manually in mouseMoveEvent because left vs right button
  // emit different mime types.
  setDragEnabled(false);
  setDragDropMode(QAbstractItemView::NoDragDrop);

  connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
    if (item == nullptr || column != kNameColumn || item->childCount() == 0) {
      return;
    }
    const bool expanded = !item->isExpanded();
    item->setExpanded(expanded);
    setDescendantsExpanded(item, expanded);
  });
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
      found = new CurveTreeItem(parent);
      found->setText(kNameColumn, part);
      found->setFlags(found->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsSelectable));
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
  auto* item = new CurveTreeItem(parent);
  item->setText(kNameColumn, leaf_name);
  item->setData(kNameColumn, Qt::UserRole, name);
  item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  sortTree();
}

void CurveTreeView::clearCurves() {
  clear();
}

void CurveTreeView::applyFilter(const QString& filter) {
  if (filter == last_filter_) {
    return;
  }
  last_filter_ = filter;
  const QStringList tokens = filter.split(' ', Qt::SkipEmptyParts);

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
  normalizeCurveNames(names);
  return names;
}

std::vector<QString> CurveTreeView::selectedCurveNamesRecursive() const {
  std::vector<QString> names;
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
    const QString full = item->data(kNameColumn, Qt::UserRole).toString();
    if (!full.isEmpty()) {
      names.push_back(full);
    }
    for (int i = 0; i < item->childCount(); ++i) {
      collect(item->child(i));
    }
  };
  for (auto* item : selectedItems()) {
    collect(item);
  }
  normalizeCurveNames(names);
  return names;
}

void CurveTreeView::setValuesColumnHidden(bool hidden) {
  setColumnHidden(kValueColumn, hidden);
}

void CurveTreeView::setDragSelectionProvider(DragSelectionProvider provider) {
  drag_selection_provider_ = std::move(provider);
}

void CurveTreeView::sortTree() {
  std::function<void(QTreeWidgetItem*)> sort_children = [&](QTreeWidgetItem* item) {
    item->sortChildren(kNameColumn, Qt::AscendingOrder);
    for (int i = 0; i < item->childCount(); ++i) {
      sort_children(item->child(i));
    }
  };
  sort_children(invisibleRootItem());
}

void CurveTreeView::setDescendantsExpanded(QTreeWidgetItem* item, bool expanded) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    if (child->childCount() > 0) {
      child->setExpanded(expanded);
      setDescendantsExpanded(child, expanded);
    }
  }
}

std::vector<QString> CurveTreeView::selectedCurveNamesForDrag() const {
  std::vector<QString> names =
      drag_selection_provider_ != nullptr ? drag_selection_provider_() : selectedCurveNamesRecursive();
  normalizeCurveNames(names);
  return names;
}

void CurveTreeView::mousePressEvent(QMouseEvent* event) {
  drag_curve_names_.clear();
  suppress_next_release_ = false;
  if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
    drag_start_pos_ = event->pos();
    drag_button_ = event->button();

    const Qt::KeyboardModifiers selection_modifiers = Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier;
    QTreeWidgetItem* item = itemAt(event->pos());
    if (item != nullptr && item->isSelected() && !(event->modifiers() & selection_modifiers)) {
      drag_curve_names_ = selectedCurveNamesForDrag();
      if (drag_curve_names_.size() > 1) {
        suppress_next_release_ = true;
        event->accept();
        return;
      }
    }
  }
  QTreeWidget::mousePressEvent(event);
}

void CurveTreeView::mouseMoveEvent(QMouseEvent* event) {
  if (drag_button_ == Qt::NoButton) {
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if (!(event->buttons() & drag_button_)) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if ((event->pos() - drag_start_pos_).manhattanLength() < QApplication::startDragDistance()) {
    if (drag_curve_names_.empty()) {
      QTreeWidget::mouseMoveEvent(event);
    } else {
      event->accept();
    }
    return;
  }

  auto names = drag_curve_names_.empty() ? selectedCurveNamesForDrag() : drag_curve_names_;
  if (names.empty()) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    QTreeWidget::mouseMoveEvent(event);
    return;
  }

  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& name : names) {
    stream << name;
  }

  auto* mime_data = new QMimeData();
  // Left-button drag → add curve to a plot; right-button drag of exactly
  // two curves → XY scatter plot. Plot-widget drop sites match on these
  // mime keys exactly.
  if (drag_button_ == Qt::LeftButton) {
    mime_data->setData("curveslist/add_curve", encoded);
  } else if (drag_button_ == Qt::RightButton && names.size() == 2) {
    mime_data->setData("curveslist/new_XY_axis", encoded);
  } else {
    delete mime_data;
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    return;
  }

  auto* drag = new QDrag(this);
  drag->setMimeData(mime_data);
  drag_button_ = Qt::NoButton;
  drag_curve_names_.clear();
  drag->exec(Qt::CopyAction | Qt::MoveAction);
}

void CurveTreeView::mouseReleaseEvent(QMouseEvent* event) {
  if (suppress_next_release_) {
    suppress_next_release_ = false;
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    event->accept();
    return;
  }
  if (event->button() == drag_button_) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
  }
  QTreeWidget::mouseReleaseEvent(event);
}

}  // namespace PJ
