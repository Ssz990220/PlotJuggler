// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/CurveTreeView.h"

#include <QApplication>
#include <QDataStream>
#include <QDrag>
#include <QFontDatabase>
#include <QHeaderView>
#include <QIcon>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QSet>
#include <QSize>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ThemeColors.h"
using namespace Qt::StringLiterals;

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
// Marks a leaf that shows a Value cell but must not be dragged or enter the drag
// payload (string fields — not plottable). See CurvePath::draggable.
constexpr int kValueOnlyRole = Qt::UserRole + 7;
// Marks an advertised-but-unsubscribed placeholder row, dimmed by the delegate.
// See CurvePath::is_placeholder.
constexpr int kPlaceholderRole = Qt::UserRole + 8;
// Marks a has-data-but-not-currently-referenced row, dimmed the same way as
// kPlaceholderRole. Set at construction from CurvePath::is_unsubscribed and
// kept live by setUnsubscribedKeys() without a rebuild.
constexpr int kUnsubscribedRole = Qt::UserRole + 9;
// Marks a topic row whose streaming is user-forced (context menu) — the
// delegate paints its name in the accent blue so a successful "Force topic
// streaming" is visible even when nothing else about the row changes. Set by
// setForcedTopicPaths() on the TOPIC node (group or leaf), never on fields.
constexpr int kForcedRole = Qt::UserRole + 10;
// Marks the managed empty-filter placeholder child row (see
// CurveTreeView::updateEmptyMessageChild) so the filter skips it and it is never
// mistaken for a real data row.
constexpr int kEmptyMessageRole = Qt::UserRole + 11;

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

// The Value column renders monospace + right-aligned so the space-padded,
// fixed-precision numbers (see formatScalarForColumn) keep their decimal points
// in the same place from row to row. Applied to every curve leaf at creation.
// Keeps `base_font`'s size/weight (the tree's font) and only swaps to a
// monospace family — a raw FixedFont renders noticeably larger than the Name
// column.
void styleValueCell(QTreeWidgetItem* item, const QFont& base_font) {
  QFont mono = base_font;
  mono.setFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  mono.setStyleHint(QFont::Monospace);
  item->setFont(kValueColumn, mono);
  item->setTextAlignment(kValueColumn, Qt::AlignRight | Qt::AlignVCenter);
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

    // Placeholder/unsubscribed flags live on the Name column's item data (see
    // addCatalogItem); fetch via the row's Name-column sibling so every column
    // of a dimmed row (Name AND Value) dims, not just whichever column is
    // being painted.
    const QModelIndex name_index = index.sibling(index.row(), kNameColumn);
    const bool dimmed = name_index.data(kPlaceholderRole).toBool() || name_index.data(kUnsubscribedRole).toBool();
    if (dimmed) {
      painter->save();
      painter->setOpacity(painter->opacity() * 0.5);
    }
    if (index.column() == kNameColumn && name_index.data(kForcedRole).toBool()) {
      // Forced streaming: accent the topic's name so the state is visible —
      // brightness alone can't distinguish "subscribed because displayed" from
      // "subscribed because forced". Name column only: the mark lives on topic
      // rows, which have no Value text, so the extra lookup is skipped on the
      // (paint-hot) Value cells.
      opt.palette.setColor(QPalette::Text, theme::kBlue);
      opt.palette.setColor(QPalette::HighlightedText, theme::kBlue);
    }

    // A topic row's builtin-family badge (image.svg / cube.svg, set as the
    // item's DecorationRole icon) is painted by the default control as a normal
    // left-side decoration, before the text; its size is the view's iconSize().
    const QWidget* widget = opt.widget;
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    if (dimmed) {
      painter->restore();
    }
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

// A value-only leaf (string field): has a Value cell but is excluded from drags.
bool isValueOnlyItem(const QTreeWidgetItem* item) {
  return item != nullptr && item->data(kNameColumn, kValueOnlyRole).toBool();
}

// Flags for a curve leaf. A draggable leaf is a normal drag source; a value-only
// leaf (string field) stays selectable/highlightable but is never dragged and is
// tagged so the selection collectors leave it out of the drag payload.
void applyLeafSelectability(QTreeWidgetItem* item, bool draggable) {
  if (draggable) {
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
    return;
  }
  item->setData(kNameColumn, kValueOnlyRole, true);
  item->setFlags((item->flags() | Qt::ItemIsSelectable) & ~Qt::ItemIsDragEnabled);
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
    icon = QIcon(loadSvg(u":/resources/svg/image.svg"_s, theme));
  } else if (is_3d_object_topic) {
    icon = QIcon(loadSvg(u":/resources/svg/cube.svg"_s, theme));
  }
  item->setIcon(kNameColumn, icon);
}

void refreshTopicIcons(QTreeWidgetItem* item, const QString& theme) {
  if (item == nullptr) {
    return;
  }
  if (item->data(kNameColumn, kImageTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(loadSvg(u":/resources/svg/image.svg"_s, theme)));
  } else if (item->data(kNameColumn, k3dObjectTopicRole).toBool()) {
    item->setIcon(kNameColumn, QIcon(loadSvg(u":/resources/svg/cube.svg"_s, theme)));
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
  // Topic-badge icons (image / 3D-object) render as the default left-side
  // decoration before the name. Size them above the 16-px small-icon default
  // so the type badge reads clearly at the left margin.
  constexpr int kTopicIconExtent = 22;
  setIconSize(QSize(kTopicIconExtent, kTopicIconExtent));
  // Curve names share long common prefixes (e.g. /robot/state_estimator/...),
  // so eliding the tail would hide exactly the part that tells two rows apart.
  // Elide the prefix instead: "…state_estimator/contact_lf". Propagates into the
  // delegate, which elides icon rows via opt.textElideMode.
  setTextElideMode(Qt::ElideLeft);
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
    if (item == nullptr || column != kNameColumn) {
      return;
    }
    if (item->childCount() == 0) {
      // Double-clicking a childless leaf is otherwise inert. A peek-eligible
      // scalar placeholder (advertised, and neither an image nor a 3D-object
      // terminal) instead requests a bounded preview so the host can land one
      // real sample and promote the row to per-field children. The expand
      // toggle below is for group nodes only, so return either way.
      const bool peek_eligible = item->data(kNameColumn, kPlaceholderRole).toBool() &&
                                 !item->data(kNameColumn, kImageTopicRole).toBool() &&
                                 !item->data(kNameColumn, k3dObjectTopicRole).toBool();
      const QString catalog_key = catalogKeyForItem(item);
      if (peek_eligible && !catalog_key.isEmpty()) {
        emit placeholderPeekRequested(catalog_key);
      }
      return;
    }
    const bool expanded = !item->isExpanded();
    item->setExpanded(expanded);
    if (item->parent() == nullptr) {
      return;
    }
    setDescendantsExpanded(item, expanded);
  });

  // The value column is filled per-tracker-tick over the *visible* rows only;
  // expanding, collapsing, or scrolling changes which rows are visible, so
  // re-apply the retained provider (deferred) to fill the newly-exposed cells.
  connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem*) { scheduleValueRefresh(); });
  connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem*) { scheduleValueRefresh(); });
  connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { scheduleValueRefresh(); });
}

QString CurveTreeView::catalogItemsMimeType() {
  return u"plotjuggler/catalog-items"_s;
}

QString CurveTreeView::newXyAxisMimeType() {
  return u"curveslist/new_XY_axis"_s;
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
      // Top-level groups are the datasets: keep them selectable so they can be
      // multi-selected for the dataset context menu (merge / remove). Intermediate
      // topic-path folders stay non-selectable. Neither is a drag source.
      const bool is_dataset = (parent == invisibleRootItem());
      Qt::ItemFlags flags = found->flags() & ~Qt::ItemIsDragEnabled;
      if (!is_dataset) {
        flags &= ~Qt::ItemIsSelectable;
      }
      found->setFlags(flags);
    }
    parent = found;
  }
  return parent;
}

QTreeWidgetItem* CurveTreeView::ensureGroup(const QString& path) {
  return ensureGroupSegments(splitPath(path));
}

void CurveTreeView::addCurve(const QString& name) {
  addCurve(name, SortMode::kImmediate);
  reapplyFilter();
}

void CurveTreeView::addCurves(const std::vector<QString>& names) {
  if (names.empty()) {
    return;
  }
  const bool updates_were_enabled = updatesEnabled();
  setUpdatesEnabled(false);
  for (const QString& name : names) {
    addCurve(name, SortMode::kDeferred);
  }
  sortTree();
  setUpdatesEnabled(updates_were_enabled);
  reapplyFilter();
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
  styleValueCell(item, font());
  item->setData(kNameColumn, Qt::UserRole, name);
  item->setData(kNameColumn, kSearchRole, name);
  item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
  if (sort_mode == SortMode::kImmediate) {
    sortTree();
  }
}

QString CurveTreeView::treePathFromCurvePath(const CurvePath& path) {
  QString tree_path = path.dataset;
  const QString topic = normalizedPathSegment(path.topic);
  const QString field = normalizedPathSegment(path.field);
  if (!topic.isEmpty()) {
    tree_path += u"/"_s + topic;
  }
  if (!field.isEmpty()) {
    tree_path += u"/"_s + field;
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
      SortMode::kImmediate);
  reapplyFilter();
}

void CurveTreeView::addCatalogItem(const CurvePath& path) {
  addCatalogItem(path, SortMode::kImmediate);
  reapplyFilter();
  expandPendingGroups();
}

void CurveTreeView::addCatalogItems(const std::vector<CurvePath>& paths) {
  if (paths.empty()) {
    return;
  }
  const bool updates_were_enabled = updatesEnabled();
  setUpdatesEnabled(false);
  for (const CurvePath& path : paths) {
    addCatalogItem(path, SortMode::kDeferred);
  }
  sortTree();
  setUpdatesEnabled(updates_were_enabled);
  reapplyFilter();
  expandPendingGroups();
  applyForcedTopicMarks();
}

void CurveTreeView::addCatalogItem(const CurvePath& path, SortMode sort_mode) {
  const QString tree_path = treePathFromCurvePath(path);
  QTreeWidgetItem* item = nullptr;
  if (view_mode_ == ViewMode::kShowTopics) {
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
      styleValueCell(item, font());
      item->setData(kNameColumn, Qt::UserRole, path.key);
      item->setData(kNameColumn, kCatalogItemRole, path.key);
      applyLeafSelectability(item, path.draggable);
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
    styleValueCell(item, font());
    item->setData(kNameColumn, Qt::UserRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    applyLeafSelectability(item, path.draggable);
  } else {
    item = ensureGroup(tree_path);
    item->setData(kNameColumn, kObjectTopicRole, path.key);
    item->setData(kNameColumn, kCatalogItemRole, path.key);
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsSelectable);
    setTopicIconDecoration(item, path.is_image_topic, path.is_3d_object_topic, currentTheme());
  }
  item->setData(kNameColumn, kSearchRole, tree_path);
  item->setData(kNameColumn, kPlaceholderRole, path.is_placeholder);
  item->setData(kNameColumn, kUnsubscribedRole, path.is_unsubscribed);
  if (sort_mode == SortMode::kImmediate) {
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

namespace {

// Separator for joined name chains: names may contain '/', so use a control
// character that never appears in topic/dataset names.
constexpr QChar kPathJoin = QChar(0x1F);

void collectExpandedPaths(const QTreeWidgetItem* item, const QString& prefix, QStringList& out) {
  for (int i = 0; i < item->childCount(); ++i) {
    const QTreeWidgetItem* child = item->child(i);
    if (child->childCount() == 0) {
      continue;
    }
    const QString path = prefix.isEmpty() ? child->text(0) : prefix + kPathJoin + child->text(0);
    if (child->isExpanded()) {
      out.push_back(path);
    }
    collectExpandedPaths(child, path, out);
  }
}

void applyExpandedPaths(QTreeWidgetItem* item, const QString& prefix, const QSet<QString>& paths) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    if (child->childCount() == 0) {
      continue;
    }
    const QString path = prefix.isEmpty() ? child->text(0) : prefix + kPathJoin + child->text(0);
    if (paths.contains(path)) {
      child->setExpanded(true);
    }
    applyExpandedPaths(child, path, paths);
  }
}

// Every row carrying a catalog key (leaf or object-topic terminal — see
// addCatalogItem, both set kCatalogItemRole) gets its unsubscribed flag set
// from membership in `unsubscribed_keys`; group/folder rows have no key and
// are left untouched.
void applyUnsubscribedFlag(QTreeWidgetItem* item, const QSet<QString>& unsubscribed_keys) {
  for (int i = 0; i < item->childCount(); ++i) {
    QTreeWidgetItem* child = item->child(i);
    const QString key = child->data(kNameColumn, kCatalogItemRole).toString();
    if (!key.isEmpty()) {
      child->setData(kNameColumn, kUnsubscribedRole, unsubscribed_keys.contains(key));
    }
    applyUnsubscribedFlag(child, unsubscribed_keys);
  }
}

const QTreeWidgetItem* findItemByCatalogKey(const QTreeWidgetItem* item, const QString& key) {
  for (int i = 0; i < item->childCount(); ++i) {
    const QTreeWidgetItem* child = item->child(i);
    if (child->data(kNameColumn, kCatalogItemRole).toString() == key) {
      return child;
    }
    if (const QTreeWidgetItem* found = findItemByCatalogKey(child, key); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

}  // namespace

QStringList CurveTreeView::expandedGroupPaths() const {
  QStringList out;
  // invisibleRootItem() is non-const in QTreeWidget; the walk only reads.
  collectExpandedPaths(const_cast<CurveTreeView*>(this)->invisibleRootItem(), QString(), out);
  return out;
}

void CurveTreeView::restoreExpandedGroupPaths(const QStringList& paths) {
  if (paths.isEmpty()) {
    return;
  }
  const QSet<QString> set(paths.begin(), paths.end());
  applyExpandedPaths(invisibleRootItem(), QString(), set);
}

void CurveTreeView::setUnsubscribedKeys(const QSet<QString>& unsubscribed_keys) {
  applyUnsubscribedFlag(invisibleRootItem(), unsubscribed_keys);
}

QString CurveTreeView::catalogKeyOf(const QTreeWidgetItem* item) {
  return catalogKeyForItem(item);
}

QStringList CurveTreeView::catalogKeysUnder(const QTreeWidgetItem* item) {
  QStringList keys;
  if (item == nullptr) {
    return keys;
  }
  const QString own = catalogKeyForItem(item);
  if (!own.isEmpty()) {
    keys.push_back(own);
  }
  for (int i = 0; i < item->childCount(); ++i) {
    keys += catalogKeysUnder(item->child(i));
  }
  return keys;
}

bool CurveTreeView::isKeyUnsubscribed(const QString& key) const {
  // invisibleRootItem() is non-const in QTreeWidget; the walk only reads.
  const QTreeWidgetItem* item = findItemByCatalogKey(const_cast<CurveTreeView*>(this)->invisibleRootItem(), key);
  return item != nullptr && item->data(kNameColumn, kUnsubscribedRole).toBool();
}

void CurveTreeView::requestExpansionWhenPromoted(const QString& tree_path) {
  if (!tree_path.isEmpty()) {
    pending_expand_paths_.insert(tree_path);
  }
}

// The topic node that a field leaf hangs under, `field_depth` tree levels above
// it, or nullptr if the walk runs off the top.
QTreeWidgetItem* topicNodeAbove(QTreeWidgetItem* leaf, int field_depth) {
  QTreeWidgetItem* node = leaf;
  for (int up = 0; up < field_depth && node != nullptr; ++up) {
    node = node->parent();
  }
  return node;
}

QTreeWidgetItem* CurveTreeView::findTopicNode(const QString& topic_path) {
  // A topic's node is either a row that IS the topic (a placeholder leaf or an
  // object-topic terminal — its search role equals the path exactly), or — for
  // a promoted scalar topic — the keyless GROUP above its field leaves: locate
  // any field leaf whose search role lives under the path, then step up the
  // field sub-path's segment count. Keying on the always-normalized search
  // role — not node text — works in both view modes.
  // INVARIANT the step-up relies on: a field leaf nests exactly one tree level
  // per '/'-segment of its field sub-path (addCatalogItem splits fields on '/'
  // in both view modes). If the tree ever groups fields differently, store an
  // explicit topic-node link on the leaves instead of counting segments.
  // KNOWN AMBIGUITY: the normalized search path cannot distinguish topic "/a"
  // with field "b/c" from topic "/a/b" with field "c" — colliding names across
  // topics can badge/expand the wrong node. Distinguishing them needs separate
  // topic/field roles on the rows; not worth it until a real source hits it.
  const QString prefix = topic_path + QLatin1Char('/');
  QTreeWidgetItem* topic_node = nullptr;
  std::function<bool(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* item) {
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      const QString search = child->data(kNameColumn, kSearchRole).toString();
      if (search == topic_path) {
        topic_node = child;
        return true;
      }
      if (child->childCount() != 0) {
        if (find(child)) {
          return true;
        }
        continue;
      }
      if (search.startsWith(prefix)) {
        const int field_depth = static_cast<int>(search.mid(prefix.size()).count(QLatin1Char('/'))) + 1;
        topic_node = topicNodeAbove(child, field_depth);
        return topic_node != nullptr;
      }
    }
    return false;
  };
  find(invisibleRootItem());
  return topic_node;
}

void CurveTreeView::expandPendingGroups() {
  if (pending_expand_paths_.isEmpty()) {
    return;
  }
  // A pending intent is honored once the placeholder promotes to a topic
  // bearing field children — a childless node (the placeholder itself) reveals
  // nothing and keeps the intent armed.
  for (const QString& topic_path : pending_expand_paths_.values()) {
    QTreeWidgetItem* topic_node = findTopicNode(topic_path);
    if (topic_node == nullptr || topic_node->childCount() == 0) {
      continue;  // not promoted yet (or single unnamed field — nothing to reveal)
    }
    for (QTreeWidgetItem* node = topic_node; node != nullptr; node = node->parent()) {
      node->setExpanded(true);
    }
    pending_expand_paths_.remove(topic_path);
  }
}

void CurveTreeView::setForcedTopicPaths(const QSet<QString>& topic_paths) {
  forced_topic_paths_ = topic_paths;
  applyForcedTopicMarks();
}

void CurveTreeView::applyForcedTopicMarks() {
  // Full-set replace: clear every mark, then set the current ones. The stored
  // set survives rebuilds (clearCurves + re-add), re-applied by addCatalogItems.
  std::function<void(QTreeWidgetItem*)> clear = [&](QTreeWidgetItem* item) {
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      if (child->data(kNameColumn, kForcedRole).toBool()) {
        child->setData(kNameColumn, kForcedRole, false);
      }
      clear(child);
    }
  };
  clear(invisibleRootItem());
  for (const QString& topic_path : forced_topic_paths_) {
    if (QTreeWidgetItem* node = findTopicNode(topic_path); node != nullptr) {
      node->setData(kNameColumn, kForcedRole, true);
    }
  }
}

bool CurveTreeView::isTopicPathForced(const QString& topic_path) {
  QTreeWidgetItem* node = findTopicNode(topic_path);
  return node != nullptr && node->data(kNameColumn, kForcedRole).toBool();
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
  refilterTree();
}

void CurveTreeView::setVisibleCurveKinds(bool show_plot, bool show_scene2d, bool show_scene3d) {
  if (show_plot_ == show_plot && show_scene2d_ == show_scene2d && show_scene3d_ == show_scene3d) {
    return;
  }
  show_plot_ = show_plot;
  show_scene2d_ = show_scene2d;
  show_scene3d_ = show_scene3d;
  refilterTree();
  viewport()->update();  // repaint rows for the new visibility (placeholder may show/hide)
}

void CurveTreeView::setEmptyFilterMessage(const QString& message) {
  if (empty_filter_message_ == message) {
    return;
  }
  empty_filter_message_ = message;
  refilterTree();  // cheap no-op on an empty tree; refreshes placeholders otherwise
}

void CurveTreeView::refilterTree() {
  const QStringList tokens = last_filter_.split(' ', Qt::SkipEmptyParts);

  // A topic's scene classification governs its ENTIRE subtree. `inherited_scene`
  // is the nearest scene-topic kind at or above `item` (kSceneNone / kScene2D /
  // kScene3D). Once the walk enters a 2D/3D topic every descendant inherits that
  // kind, so hiding the kind collapses the whole topic — its scalar fields
  // included — and those fields never fall into the Plot bucket. A row with no
  // scene ancestor is Plot ("by exclusion") when it bears a catalog key, or a
  // kind-less folder (revealed only by a visible descendant) when it doesn't.
  enum : int { kSceneNone = 0, kScene2D = 2, kScene3D = 3 };
  std::function<bool(QTreeWidgetItem*, int)> apply = [&](QTreeWidgetItem* item, int inherited_scene) {
    int scene = inherited_scene;
    if (scene == kSceneNone) {
      if (item->data(kNameColumn, kImageTopicRole).toBool()) {
        scene = kScene2D;
      } else if (item->data(kNameColumn, k3dObjectTopicRole).toBool()) {
        scene = kScene3D;
      }
    }
    bool any_child_visible = false;
    for (int i = 0; i < item->childCount(); ++i) {
      QTreeWidgetItem* child = item->child(i);
      if (child->data(kNameColumn, kEmptyMessageRole).toBool()) {
        continue;  // the managed empty-filter placeholder is not real data
      }
      any_child_visible = apply(child, scene) || any_child_visible;
    }
    bool type_ok = false;
    if (scene == kScene2D) {
      type_ok = show_scene2d_;
    } else if (scene == kScene3D) {
      type_ok = show_scene3d_;
    } else {
      // kCatalogItemRole is only ever set to a (non-empty) key, so its presence
      // marks a data-bearing row; a keyless folder rides on its children.
      const bool has_key = item->data(kNameColumn, kCatalogItemRole).isValid();
      type_ok = has_key && show_plot_;
    }
    QString haystack = item->data(kNameColumn, kSearchRole).toString();
    if (haystack.isEmpty()) {
      const QString full = item->data(kNameColumn, Qt::UserRole).toString();
      haystack = full.isEmpty() ? item->text(kNameColumn) : full;
    }
    const bool text_match = std::all_of(tokens.begin(), tokens.end(), [&](const QString& token) {
      return haystack.contains(token, Qt::CaseInsensitive);
    });
    const bool visible = any_child_visible || (text_match && type_ok);
    item->setHidden(!visible);
    return visible;
  };

  for (int i = 0; i < topLevelItemCount(); ++i) {
    QTreeWidgetItem* dataset_node = topLevelItem(i);
    const bool visible = apply(dataset_node, kSceneNone);
    updateEmptyMessageChild(dataset_node, !visible);
  }
}

void CurveTreeView::reapplyFilter() {
  const bool type_restricted = !(show_plot_ && show_scene2d_ && show_scene3d_);
  if (last_filter_.isEmpty() && !type_restricted) {
    return;  // no active filter: freshly inserted rows are visible by default
  }
  refilterTree();
}

void CurveTreeView::updateEmptyMessageChild(QTreeWidgetItem* dataset_node, bool subtree_hidden) {
  // Find any existing placeholder + learn whether the dataset has real topics.
  QTreeWidgetItem* placeholder = nullptr;
  bool has_real_child = false;
  for (int i = 0; i < dataset_node->childCount(); ++i) {
    QTreeWidgetItem* child = dataset_node->child(i);
    if (child->data(kNameColumn, kEmptyMessageRole).toBool()) {
      placeholder = child;
    } else {
      has_real_child = true;
    }
  }

  // Only speak up when a dataset that HAS topics has them all filtered away.
  const bool want_message = subtree_hidden && has_real_child && !empty_filter_message_.isEmpty();
  if (!want_message) {
    if (placeholder != nullptr) {
      placeholder->setHidden(true);
    }
    return;
  }

  if (placeholder == nullptr) {
    placeholder = new QTreeWidgetItem(dataset_node);
    placeholder->setData(kNameColumn, kEmptyMessageRole, true);
    placeholder->setFlags(Qt::ItemIsEnabled);  // display-only: not selectable/draggable
    QFont message_font = font();
    message_font.setItalic(true);
    placeholder->setFont(kNameColumn, message_font);
    placeholder->setForeground(kNameColumn, palette().color(QPalette::Disabled, QPalette::Text));
  }
  placeholder->setText(kNameColumn, empty_filter_message_);
  placeholder->setHidden(false);
  // Span the message across both columns; re-assert each call since a re-sort can
  // move the placeholder to a different child row (spanning is keyed by row).
  setFirstColumnSpanned(dataset_node->indexOfChild(placeholder), indexFromItem(dataset_node), true);
  dataset_node->setHidden(false);   // keep the dataset name visible
  dataset_node->setExpanded(true);  // reveal the message row
}

std::vector<QString> CurveTreeView::selectedCurveNames() const {
  std::vector<QString> names;
  for (auto* item : selectedItems()) {
    if (isValueOnlyItem(item)) {
      continue;  // string fields are not draggable curves
    }
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
    if (!full.isEmpty() && !isValueOnlyItem(item)) {
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
    if (!key.isEmpty() && !isValueOnlyItem(item)) {
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
  if (!hidden) {
    scheduleValueRefresh();  // re-show stale cells when the column reappears
  }
}

void CurveTreeView::refreshVisibleValues(const std::function<QString(const QString&)>& value_provider) {
  value_provider_ = value_provider;
  applyVisibleValues();
}

void CurveTreeView::applyVisibleValues() {
  if (isColumnHidden(kValueColumn) || !value_provider_) {
    return;
  }
  // Visit every item but cull each row independently by its rect — only the
  // on-screen leaves are written, so the expensive part (the per-leaf lookup
  // inside value_provider_) stays O(visible). The walk itself is O(N) cheap
  // geometry checks. Matches PlotJuggler 3's curve-list refresh: culling each
  // row (rather than breaking at the first off-screen row) is what keeps
  // scrolled-in rows correct in a deeply-nested tree.
  const int viewport_height = viewport()->height();
  for (int i = 0; i < topLevelItemCount(); ++i) {
    applyVisibleValuesToSubtree(topLevelItem(i), viewport_height);
  }
}

void CurveTreeView::applyVisibleValuesToSubtree(QTreeWidgetItem* item, int viewport_height) {
  if (item->childCount() != 0) {
    for (int i = 0; i < item->childCount(); ++i) {
      applyVisibleValuesToSubtree(item->child(i), viewport_height);
    }
    return;
  }
  const QString key = catalogKeyForItem(item);
  if (key.isEmpty()) {
    return;  // group placeholder / unkeyed row
  }
  const QRect rect = visualItemRect(item);
  if (rect.isNull() || rect.bottom() < 0 || rect.top() > viewport_height) {
    return;  // off-screen or collapsed — skip the value lookup entirely
  }
  const QString text = value_provider_(key);
  if (text == item->text(kValueColumn)) {
    return;  // value unchanged — skip setText so a stable cell emits no
             // dataChanged / repaint (the bulk of the value column's CPU at 10 Hz)
  }
  item->setText(kValueColumn, text);
  // Full value as a tooltip so a string left-elided in the narrow Value column
  // (or a long number) stays readable on hover; trimmed of the alignment padding.
  item->setToolTip(kValueColumn, text.trimmed());
}

void CurveTreeView::scheduleValueRefresh() {
  // This coalesces VISIBILITY-driven re-applies (expand/collapse/scroll/resize)
  // onto the next event-loop turn; the TRACKER-driven refresh rate is capped
  // separately by the owner (CurveListPanel's 10 Hz throttle). Deferring matters
  // because when this fires from an itemExpanded / scroll handler the freshly-
  // revealed rows have not been laid out yet, so reading visualItemRect now would
  // wrongly cull them.
  if (value_refresh_scheduled_ || !value_provider_ || isColumnHidden(kValueColumn)) {
    return;
  }
  value_refresh_scheduled_ = true;
  QTimer::singleShot(0, this, [this]() {
    value_refresh_scheduled_ = false;
    applyVisibleValues();
  });
}

void CurveTreeView::resizeEvent(QResizeEvent* event) {
  QTreeWidget::resizeEvent(event);
  syncNameColumnWidth();
  scheduleValueRefresh();  // a taller viewport exposes more rows to fill
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
  drag_button_ = Qt::NoButton;
  if (event->button() == Qt::LeftButton || event->button() == Qt::RightButton) {
    QTreeWidgetItem* item = itemAt(event->pos());
    // Only draggable rows (curve leaves / object topics) initiate a drag or the
    // drag-the-whole-selection gesture. Non-draggable rows — notably the selectable
    // dataset groups — fall straight through to the base handler, so plain/Ctrl/Shift
    // selection and expand/collapse behave normally and a dataset never starts a drag.
    const bool draggable = item != nullptr && (item->flags() & Qt::ItemIsDragEnabled);
    if (draggable) {
      drag_start_pos_ = event->pos();
      drag_button_ = event->button();

      const Qt::KeyboardModifiers selection_modifiers = Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier;
      const QString item_catalog_key = catalogKeyForItem(item);
      if (!item_catalog_key.isEmpty()) {
        drag_catalog_keys_.push_back(item_catalog_key);
      }
      if (item->isSelected() && !(event->modifiers() & selection_modifiers)) {
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
    mime_data->setData(u"curveslist/add_curve"_s, encoded);
  } else if (right_xy) {
    mime_data->setData(newXyAxisMimeType(), encoded);
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

QString formatScalarForColumn(double value, int precision) {
  if (!std::isfinite(value)) {
    return u"-"_s;
  }
  // Fixed precision keeps a constant fractional width; then overwrite trailing
  // zeros — and a bare trailing '.' once all decimals are blanked — with spaces.
  // The single appended space + a right-aligned monospace cell line up every
  // decimal point across rows (e.g. 1.2 -> "1.2   ", 5 -> "5     ").
  QString text = QString::number(value, 'f', precision);
  const int dot = text.indexOf(QLatin1Char('.'));
  if (dot >= 0) {
    int idx = text.size() - 1;
    while (idx > dot && text[idx] == QLatin1Char('0')) {
      text[idx] = QLatin1Char(' ');
      --idx;
    }
    if (idx == dot) {
      text[idx] = QLatin1Char(' ');
    }
  }
  return text + QLatin1Char(' ');
}

}  // namespace PJ
