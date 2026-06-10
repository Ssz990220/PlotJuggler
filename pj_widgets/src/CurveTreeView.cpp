// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CurveTreeView.h"

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QHeaderView>
#include <QIcon>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <algorithm>
#include <functional>
#include <utility>

#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {
constexpr int kNameColumn = 0;
constexpr int kValueColumn = 1;
constexpr int kSearchRole = Qt::UserRole + 1;
constexpr int kObjectTopicRole = Qt::UserRole + 2;
constexpr int kCatalogItemRole = Qt::UserRole + 3;
constexpr int kImageTopicRole = Qt::UserRole + 4;
constexpr int k3dObjectTopicRole = Qt::UserRole + 5;
constexpr int kSortKeyRole = Qt::UserRole + 6;

QStringList splitPath(const QString& name) {
  return name.split('/', Qt::SkipEmptyParts);
}

QString normalizedPathSegment(QString path) {
  path.replace('.', '/');
  while (path.startsWith('/')) {
    path.remove(0, 1);
  }
  return path;
}

void setItemName(QTreeWidgetItem* item, const QString& name) {
  item->setText(kNameColumn, name);
  item->setData(kNameColumn, kSortKeyRole, name.toCaseFolded());
}

QString sortKeyForItem(const QTreeWidgetItem& item) {
  const QString cached_key = item.data(kNameColumn, kSortKeyRole).toString();
  if (!cached_key.isEmpty() || item.text(kNameColumn).isEmpty()) {
    return cached_key;
  }
  return item.text(kNameColumn).toCaseFolded();
}

class CurveTreeItem : public QTreeWidgetItem {
 public:
  explicit CurveTreeItem(QTreeWidgetItem* parent) : QTreeWidgetItem(parent) {}

  bool operator<(const QTreeWidgetItem& other) const override {
    const QString lhs_key = sortKeyForItem(*this);
    const QString rhs_key = sortKeyForItem(other);
    const int folded_compare = QString::localeAwareCompare(lhs_key, rhs_key);
    if (folded_compare != 0) {
      return folded_compare < 0;
    }
    return QString::localeAwareCompare(text(kNameColumn), other.text(kNameColumn)) < 0;
  }
};

class CurveTreeItemDelegate : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    // A topic row may carry one of two trailing icons: image.svg for
    // image-family topics, cube.svg for 3D-object topics. We
    // suppress the standard left-side decoration and paint the icon at
    // the right edge of the elided text. The two roles are mutually
    // exclusive in practice (a topic is one builtin family), so we pick
    // whichever role is set; image takes precedence if both happen.
    const bool draw_image_icon = index.column() == kNameColumn && index.data(kImageTopicRole).toBool();
    const bool draw_3d_object_icon =
        !draw_image_icon && index.column() == kNameColumn && index.data(k3dObjectTopicRole).toBool();
    const bool draw_trailing_icon = draw_image_icon || draw_3d_object_icon;
    const QIcon trailing_icon = opt.icon;
    const QWidget* widget = opt.widget;
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    QRect trailing_icon_rect;
    if (draw_trailing_icon) {
      constexpr int kIconExtent = 16;
      constexpr int kIconMargin = 4;
      opt.icon = {};
      opt.features &= ~QStyleOptionViewItem::HasDecoration;

      const QRect text_rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
      opt.text = opt.fontMetrics.elidedText(
          opt.text, opt.textElideMode, std::max(0, text_rect.width() - kIconExtent - kIconMargin));
      const int icon_left = std::min(
          text_rect.left() + opt.fontMetrics.horizontalAdvance(opt.text) + kIconMargin,
          text_rect.right() - kIconExtent + 1);
      trailing_icon_rect = QRect(icon_left, option.rect.center().y() - (kIconExtent / 2), kIconExtent, kIconExtent);
    }

    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    if (!draw_trailing_icon || trailing_icon.isNull()) {
      return;
    }

    const QIcon::Mode mode = option.state.testFlag(QStyle::State_Enabled) ? QIcon::Normal : QIcon::Disabled;
    const QIcon::State state = option.state.testFlag(QStyle::State_Open) ? QIcon::On : QIcon::Off;
    trailing_icon.paint(painter, trailing_icon_rect, Qt::AlignCenter, mode, state);
  }
};

void normalizeCurveNames(std::vector<QString>& names) {
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

QString curveNameForItem(const QTreeWidgetItem* item) {
  if (item == nullptr) {
    return {};
  }
  return item->data(kNameColumn, Qt::UserRole).toString();
}

bool isObjectTopicItem(const QTreeWidgetItem* item) {
  return item != nullptr && !item->data(kNameColumn, kObjectTopicRole).toString().isEmpty();
}

QString catalogKeyForItem(const QTreeWidgetItem* item) {
  if (item == nullptr) {
    return {};
  }
  QString key = item->data(kNameColumn, kCatalogItemRole).toString();
  if (!key.isEmpty()) {
    return key;
  }
  key = item->data(kNameColumn, Qt::UserRole).toString();
  if (!key.isEmpty()) {
    return key;
  }
  return item->data(kNameColumn, kObjectTopicRole).toString();
}

void setTopicIconDecoration(QTreeWidgetItem* item, bool is_image_topic, bool is_3d_object_topic, const QString& theme) {
  if (item == nullptr) {
    return;
  }
  item->setData(kNameColumn, kImageTopicRole, is_image_topic);
  item->setData(kNameColumn, k3dObjectTopicRole, is_3d_object_topic);
  QIcon icon;
  if (is_image_topic) {
    icon = QIcon(LoadSvg(QStringLiteral(":/resources/svg/image.svg"), theme));
  } else if (is_3d_object_topic) {
    icon = QIcon(LoadSvg(QStringLiteral(":/resources/svg/cube.svg"), theme));
  }
  item->setIcon(kNameColumn, icon);
}

void refreshTopicIcons(QTreeWidgetItem* item, const QString& theme) {
  if (item == nullptr) {
    return;
  }
  if (item->data(kNameColumn, kImageTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(LoadSvg(QStringLiteral(":/resources/svg/image.svg"), theme)));
  } else if (item->data(kNameColumn, k3dObjectTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(LoadSvg(QStringLiteral(":/resources/svg/cube.svg"), theme)));
  }
  for (int i = 0; i < item->childCount(); ++i) {
    refreshTopicIcons(item->child(i), theme);
  }
}
}  // namespace

CurveTreeView::CurveTreeView(QWidget* parent) : QTreeWidget(parent) {
  setColumnCount(2);
  setHeaderLabels({tr("Name"), tr("Value")});
  setItemDelegate(new CurveTreeItemDelegate(this));
  // Splitter-style divider: both sections Interactive (Stretch sections
  // refuse to yield space, so the divider next to a Stretch section is
  // not draggable). When the user drags the divider Qt resizes Name
  // (left of divider); we mirror the change into Value so the two
  // sections together always fill the viewport. Default Value width is
  // narrow — the numeric column shouldn't dominate the panel.
  header()->setSectionResizeMode(kNameColumn, QHeaderView::Interactive);
  header()->setSectionResizeMode(kValueColumn, QHeaderView::Interactive);
  header()->setStretchLastSection(false);
  header()->setMinimumSectionSize(20);
  header()->resizeSection(kValueColumn, 60);
  header()->setSectionsClickable(false);
  // Ensure the header sees Enter/Leave/HoverMove events — the QSS
  // `QHeaderView::section:hover` rule that tints the column divider
  // purple only fires when the header has the Hover attribute set.
  header()->setAttribute(Qt::WA_Hover, true);
  header()->viewport()->setAttribute(Qt::WA_Hover, true);
  // Splitter behavior: dragging the divider resizes Name; we translate
  // that into a Value resize (delta in opposite direction) so the
  // divider acts like a QSplitter handle and Name + Value always equal
  // the viewport width. The reentry guard stops the programmatic Value
  // resize from cascading back through this same handler.
  connect(header(), &QHeaderView::sectionResized, this, [this](int section, int old_size, int new_size) {
    if (adjusting_columns_ || section != kNameColumn) {
      return;
    }
    const int delta = new_size - old_size;
    const int target_value =
        std::clamp(columnWidth(kValueColumn) - delta, header()->minimumSectionSize(), viewport()->width());
    adjusting_columns_ = true;
    header()->resizeSection(kValueColumn, target_value);
    // If Value clamped (hit min or max), the delta consumed by Value
    // is less than the user's drag; rebase Name so total = viewport.
    syncNameColumnWidth();
    adjusting_columns_ = false;
  });
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
    if (item->parent() == nullptr) {
      return;
    }
    setDescendantsExpanded(item, expanded);
  });
}

QString CurveTreeView::catalogItemsMimeType() {
  return QStringLiteral("plotjuggler/catalog-items");
}

QByteArray CurveTreeView::encodeCatalogKeys(const QStringList& keys) {
  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& key : keys) {
    if (!key.isEmpty()) {
      stream << key;
    }
  }
  return encoded;
}

QStringList CurveTreeView::decodeCatalogKeys(const QMimeData* mime_data) {
  QStringList keys;
  if (mime_data == nullptr || !mime_data->hasFormat(catalogItemsMimeType())) {
    return keys;
  }

  QByteArray encoded = mime_data->data(catalogItemsMimeType());
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  while (!stream.atEnd()) {
    QString key;
    stream >> key;
    if (!key.isEmpty()) {
      keys.push_back(key);
    }
  }
  keys.removeDuplicates();
  return keys;
}

QTreeWidgetItem* CurveTreeView::ensureGroupSegments(const QStringList& segments) {
  QTreeWidgetItem* parent = invisibleRootItem();
  for (const QString& part : segments) {
    if (part.isEmpty()) {
      continue;
    }
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
      setItemName(found, part);
      found->setFlags(found->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsSelectable));
    }
    parent = found;
  }
  return parent;
}

QTreeWidgetItem* CurveTreeView::ensureGroup(const QString& path) {
  return ensureGroupSegments(splitPath(path));
}

void CurveTreeView::addCurve(const QString& name) {
  addCurve(name, SortMode::Immediate);
}

void CurveTreeView::addCurves(const std::vector<QString>& names) {
  if (names.empty()) {
    return;
  }
  const bool updates_were_enabled = updatesEnabled();
  setUpdatesEnabled(false);
  for (const QString& name : names) {
    addCurve(name, SortMode::Deferred);
  }
  sortTree();
  setUpdatesEnabled(updates_were_enabled);
}

void CurveTreeView::addCurve(const QString& name, SortMode sort_mode) {
  const int last_sep = name.lastIndexOf('/');
  QTreeWidgetItem* parent = invisibleRootItem();
  QString leaf_name = name;
  if (last_sep >= 0) {
    parent = ensureGroup(name.left(last_sep));
    leaf_name = name.mid(last_sep + 1);
  }
  auto* item = new CurveTreeItem(parent);
  setItemName(item, leaf_name);
  item->setData(kNameColumn, Qt::UserRole, name);
  item->setData(kNameColumn, kSearchRole, name);
  item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  if (sort_mode == SortMode::Immediate) {
    sortTree();
  }
}

QString CurveTreeView::treePathFromCurvePath(const CurvePath& path) const {
  QString tree_path = path.dataset;
  const QString topic = normalizedPathSegment(path.topic);
  const QString field = normalizedPathSegment(path.field);
  if (!topic.isEmpty()) {
    tree_path += QStringLiteral("/") + topic;
  }
  if (!field.isEmpty()) {
    tree_path += QStringLiteral("/") + field;
  }
  return tree_path;
}

void CurveTreeView::addCurve(const CurvePath& path) {
  addCatalogItem(
      CurvePath{
          .key = path.key,
          .dataset = path.dataset,
          .topic = path.topic,
          .field = path.field,
          .selectable = true,
          .is_image_topic = false,
          .is_3d_object_topic = false,
      },
      SortMode::Immediate);
}

void CurveTreeView::addCatalogItem(const CurvePath& path) {
  addCatalogItem(path, SortMode::Immediate);
}

void CurveTreeView::addCatalogItems(const std::vector<CurvePath>& paths) {
  if (paths.empty()) {
    return;
  }
  const bool updates_were_enabled = updatesEnabled();
  setUpdatesEnabled(false);
  for (const CurvePath& path : paths) {
    addCatalogItem(path, SortMode::Deferred);
  }
  sortTree();
  setUpdatesEnabled(updates_were_enabled);
}

void CurveTreeView::addCatalogItem(const CurvePath& path, SortMode sort_mode) {
  const QString tree_path = treePathFromCurvePath(path);
  QTreeWidgetItem* item = nullptr;
  if (view_mode_ == ViewMode::ShowTopics) {
    // Dataset and topic are atomic (topic shown verbatim). The field still
    // splits on '/' after '.' → '/' so nested struct fields fan out as
    // sub-folders under the topic node.
    QStringList segments;
    if (!path.dataset.isEmpty()) {
      segments << path.dataset;
    }
    if (!path.topic.isEmpty()) {
      segments << path.topic;
    }
    if (path.selectable) {
      segments += splitPath(normalizedPathSegment(path.field));
      QString leaf_name = segments.isEmpty() ? tree_path : segments.takeLast();
      QTreeWidgetItem* parent = ensureGroupSegments(segments);
      item = new CurveTreeItem(parent);
      setItemName(item, leaf_name);
      item->setData(kNameColumn, Qt::UserRole, path.key);
      item->setData(kNameColumn, kCatalogItemRole, path.key);
      item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
    } else {
      // Object topic: terminal at dataset ▸ topic.
      item = ensureGroupSegments(segments);
      item->setData(kNameColumn, kObjectTopicRole, path.key);
      item->setData(kNameColumn, kCatalogItemRole, path.key);
      item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
      setTopicIconDecoration(item, path.is_image_topic, path.is_3d_object_topic, currentTheme());
    }
  } else if (path.selectable) {
    const int last_sep = tree_path.lastIndexOf('/');
    QTreeWidgetItem* parent = invisibleRootItem();
    QString leaf_name = tree_path;
    if (last_sep >= 0) {
      parent = ensureGroup(tree_path.left(last_sep));
      leaf_name = tree_path.mid(last_sep + 1);
    }
    item = new CurveTreeItem(parent);
    setItemName(item, leaf_name);
    item->setData(kNameColumn, Qt::UserRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  } else {
    item = ensureGroup(tree_path);
    item->setData(kNameColumn, kObjectTopicRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
    setTopicIconDecoration(item, path.is_image_topic, path.is_3d_object_topic, currentTheme());
  }
  item->setData(kNameColumn, kSearchRole, tree_path);
  if (sort_mode == SortMode::Immediate) {
    sortTree();
  }
}

void CurveTreeView::setViewMode(ViewMode mode) {
  if (view_mode_ == mode) {
    return;
  }
  view_mode_ = mode;
  // Force the next applyFilter() call to actually re-run, otherwise the
  // text-equality short-circuit at the top of applyFilter() would skip the
  // re-filter that the post-rebuild caller expects.
  last_filter_.clear();
}

void CurveTreeView::clearCurves() {
  clear();
}

void CurveTreeView::refreshIcons(const QString& theme) {
  for (int i = 0; i < topLevelItemCount(); ++i) {
    refreshTopicIcons(topLevelItem(i), theme);
  }
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
    QString haystack = item->data(kNameColumn, kSearchRole).toString();
    if (haystack.isEmpty()) {
      const QString full = item->data(kNameColumn, Qt::UserRole).toString();
      haystack = full.isEmpty() ? item->text(kNameColumn) : full;
    }
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
    const QString full = curveNameForItem(item);
    if (!full.isEmpty()) {
      names.push_back(full);
    }
    if (isObjectTopicItem(item)) {
      return;
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

std::vector<QString> CurveTreeView::selectedCatalogKeysRecursive() const {
  std::vector<QString> keys;
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* item) {
    const QString key = catalogKeyForItem(item);
    if (!key.isEmpty()) {
      keys.push_back(key);
    }
    if (isObjectTopicItem(item)) {
      return;
    }
    for (int i = 0; i < item->childCount(); ++i) {
      collect(item->child(i));
    }
  };
  for (auto* item : selectedItems()) {
    collect(item);
  }
  normalizeCurveNames(keys);
  return keys;
}

void CurveTreeView::setValuesColumnHidden(bool hidden) {
  setColumnHidden(kValueColumn, hidden);
  syncNameColumnWidth();
}

void CurveTreeView::resizeEvent(QResizeEvent* event) {
  QTreeWidget::resizeEvent(event);
  syncNameColumnWidth();
}

void CurveTreeView::syncNameColumnWidth() {
  // Name fills whatever's left after the Value column. The viewport
  // width excludes the vertical scrollbar, so Name + Value always sum
  // to exactly the visible row width — no dead space, no horizontal
  // scroll triggered by the header.
  const int viewport_width = viewport()->width();
  const int value_width = isColumnHidden(kValueColumn) ? 0 : columnWidth(kValueColumn);
  const int min_section = header()->minimumSectionSize();
  const int name_width = std::max(min_section, viewport_width - value_width);
  if (columnWidth(kNameColumn) == name_width) {
    return;
  }
  // Suppress the sectionResized hijack: this is a programmatic sync of
  // Name to the viewport, not a user drag, so it must not be re-routed
  // back into a Value resize. Without the guard the resize event would
  // cascade infinitely (sync → resizeSection → lambda → sync → ...).
  const bool was_adjusting = adjusting_columns_;
  adjusting_columns_ = true;
  header()->resizeSection(kNameColumn, name_width);
  adjusting_columns_ = was_adjusting;
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
  drag_catalog_keys_.clear();
  suppress_next_release_ = false;
  if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
    drag_start_pos_ = event->pos();
    drag_button_ = event->button();

    const Qt::KeyboardModifiers selection_modifiers = Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier;
    QTreeWidgetItem* item = itemAt(event->pos());
    const QString item_catalog_key = catalogKeyForItem(item);
    if (!item_catalog_key.isEmpty()) {
      drag_catalog_keys_.push_back(item_catalog_key);
    }
    if (item != nullptr && item->isSelected() && !(event->modifiers() & selection_modifiers)) {
      drag_curve_names_ = selectedCurveNamesForDrag();
      // A plain press on an already-selected row drags the WHOLE selection.
      // Preserve it (suppress the release that would otherwise collapse it to
      // the clicked row) whenever more than one row is selected — counting
      // object/image topics too, which contribute catalog keys but no scalar
      // curve names. The payload itself is read back from the live selection in
      // createDragMimeData(), so it stays correct as long as we keep it intact.
      if (drag_curve_names_.size() > 1 || selectedCatalogKeysRecursive().size() > 1) {
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
    drag_catalog_keys_.clear();
    QTreeWidget::mouseMoveEvent(event);
    return;
  }
  if ((event->pos() - drag_start_pos_).manhattanLength() < QApplication::startDragDistance()) {
    if (drag_curve_names_.empty() && drag_catalog_keys_.empty()) {
      QTreeWidget::mouseMoveEvent(event);
    } else {
      event->accept();
    }
    return;
  }

  QMimeData* mime_data = createDragMimeData(drag_button_);
  drag_button_ = Qt::NoButton;
  drag_curve_names_.clear();
  drag_catalog_keys_.clear();
  if (mime_data == nullptr) {
    return;
  }

  auto* drag = new QDrag(this);
  drag->setMimeData(mime_data);
  drag->exec(Qt::CopyAction | Qt::MoveAction);
}

QMimeData* CurveTreeView::createDragMimeData(Qt::MouseButton button) const {
  const std::vector<QString> names = selectedCurveNamesForDrag();

  // The catalog payload must carry EVERY selected item, not just the row under
  // the cursor when the drag began. Object/image topics come from the recursive
  // catalog walk; scalar curves (whose catalog key is their own name) are added
  // from `names`, which also folds in a cross-view selection supplied by a drag
  // selection provider.
  QStringList catalog_keys;
  for (const QString& key : selectedCatalogKeysRecursive()) {
    catalog_keys.push_back(key);
  }
  for (const QString& name : names) {
    catalog_keys.push_back(name);
  }
  catalog_keys.removeDuplicates();

  // Left-button drag → add curve(s) to a plot; right-button drag of exactly two
  // curves → XY scatter plot. Anything else is not a drag we initiate.
  const bool left_add = button == Qt::LeftButton;
  const bool right_xy = button == Qt::RightButton && names.size() == 2;
  if ((!left_add && !right_xy) || (names.empty() && catalog_keys.empty())) {
    return nullptr;
  }

  QByteArray encoded;
  QDataStream stream(&encoded, QIODevice::WriteOnly);
  for (const QString& name : names) {
    stream << name;
  }

  auto* mime_data = new QMimeData();
  // Plot-widget and placeholder drop sites match on these mime keys exactly.
  if (!catalog_keys.empty()) {
    mime_data->setData(catalogItemsMimeType(), encodeCatalogKeys(catalog_keys));
  }
  if (left_add && !names.empty()) {
    mime_data->setData(QStringLiteral("curveslist/add_curve"), encoded);
  } else if (right_xy) {
    mime_data->setData(QStringLiteral("curveslist/new_XY_axis"), encoded);
  }
  return mime_data;
}

void CurveTreeView::mouseReleaseEvent(QMouseEvent* event) {
  if (suppress_next_release_) {
    suppress_next_release_ = false;
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    drag_catalog_keys_.clear();
    event->accept();
    return;
  }
  if (event->button() == drag_button_) {
    drag_button_ = Qt::NoButton;
    drag_curve_names_.clear();
    drag_catalog_keys_.clear();
  }
  QTreeWidget::mouseReleaseEvent(event);
}

}  // namespace PJ
