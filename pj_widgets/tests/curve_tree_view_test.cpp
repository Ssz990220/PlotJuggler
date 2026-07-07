// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTreeWidgetItem>
#include <QtGlobal>
#include <memory>
#include <string>
#include <vector>

#include "pj_widgets/CurveTreeView.h"

namespace {

class TestCurveTreeView : public PJ::CurveTreeView {
 public:
  using PJ::CurveTreeView::mousePressEvent;
  using PJ::CurveTreeView::mouseReleaseEvent;
};

std::vector<std::string> toStdStrings(const std::vector<QString>& names) {
  std::vector<std::string> result;
  result.reserve(names.size());
  for (const QString& name : names) {
    result.push_back(name.toStdString());
  }
  return result;
}

std::vector<std::string> topLevelNames(const PJ::CurveTreeView& view) {
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(view.topLevelItemCount()));
  for (int i = 0; i < view.topLevelItemCount(); ++i) {
    names.push_back(view.topLevelItem(i)->text(0).toStdString());
  }
  return names;
}

std::vector<std::string> childNames(const QTreeWidgetItem* item) {
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(item->childCount()));
  for (int i = 0; i < item->childCount(); ++i) {
    names.push_back(item->child(i)->text(0).toStdString());
  }
  return names;
}

QTreeWidgetItem* findChild(QTreeWidgetItem* parent, const QString& name) {
  if (parent == nullptr) {
    return nullptr;
  }
  for (int i = 0; i < parent->childCount(); ++i) {
    if (parent->child(i)->text(0) == name) {
      return parent->child(i);
    }
  }
  return nullptr;
}

}  // namespace

TEST(CurveTreeViewTest, SortsTopLevelGroupsAndChildren) {
  PJ::CurveTreeView view;

  view.addCurve(QStringLiteral("gamma/zeta"));
  view.addCurve(QStringLiteral("alpha/delta"));
  view.addCurve(QStringLiteral("beta/root"));
  view.addCurve(QStringLiteral("alpha/charlie"));
  view.addCurve(QStringLiteral("alpha/bravo"));

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), QStringLiteral("alpha"));
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

TEST(CurveTreeViewTest, BatchedCatalogInsertSortsTopLevelGroupsAndChildren) {
  PJ::CurveTreeView view;

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("gamma/zeta"),
          .dataset = QStringLiteral("gamma"),
          .topic = {},
          .field = QStringLiteral("zeta"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("alpha/delta"),
          .dataset = QStringLiteral("alpha"),
          .topic = {},
          .field = QStringLiteral("delta"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("beta/root"),
          .dataset = QStringLiteral("beta"),
          .topic = {},
          .field = QStringLiteral("root"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("alpha/charlie"),
          .dataset = QStringLiteral("alpha"),
          .topic = {},
          .field = QStringLiteral("charlie"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("alpha/bravo"),
          .dataset = QStringLiteral("alpha"),
          .topic = {},
          .field = QStringLiteral("bravo"),
      },
  });

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), QStringLiteral("alpha"));
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

// A filter typed BEFORE data is loaded must apply to the rows that arrive later
// (the catalog-insert path), not just to rows already in the tree.
TEST(CurveTreeViewTest, FilterAppliesToRowsInsertedAfterItWasSet) {
  PJ::CurveTreeView view;

  view.applyFilter(QStringLiteral("imu"));

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/imu/x"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("imu"),
          .field = QStringLiteral("x"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("veh/gps/lat"),
          .dataset = QStringLiteral("veh"),
          .topic = QStringLiteral("gps"),
          .field = QStringLiteral("lat"),
      },
  });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* veh = view.topLevelItem(0);
  EXPECT_FALSE(veh->isHidden());

  QTreeWidgetItem* imu = findChild(veh, QStringLiteral("imu"));
  ASSERT_NE(imu, nullptr);
  EXPECT_FALSE(imu->isHidden());
  EXPECT_FALSE(findChild(imu, QStringLiteral("x"))->isHidden());

  QTreeWidgetItem* gps = findChild(veh, QStringLiteral("gps"));
  ASSERT_NE(gps, nullptr);
  EXPECT_TRUE(gps->isHidden());
  EXPECT_TRUE(findChild(gps, QStringLiteral("lat"))->isHidden());
}

TEST(CurveTreeViewTest, TopLevelGroupsSelectableIntermediateGroupsLeafOnly) {
  PJ::CurveTreeView view;

  // Two-level path: "dataset" (top-level group) / "folder" (intermediate) / leaf.
  view.addCurve(QStringLiteral("dataset/folder/b"));
  view.addCurve(QStringLiteral("dataset/folder/a"));

  ASSERT_EQ(view.selectionMode(), QAbstractItemView::ExtendedSelection);
  ASSERT_EQ(view.selectionBehavior(), QAbstractItemView::SelectRows);

  // Top-level groups are datasets: selectable (so they can be multi-selected for
  // the merge / remove context menu) but never drag sources.
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  EXPECT_TRUE(dataset->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(dataset->flags().testFlag(Qt::ItemIsDragEnabled));

  // Intermediate (topic-path) folders stay non-selectable + non-draggable.
  ASSERT_EQ(dataset->childCount(), 1);
  QTreeWidgetItem* folder = dataset->child(0);
  EXPECT_FALSE(folder->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(folder->flags().testFlag(Qt::ItemIsDragEnabled));

  // Leaves remain selectable + draggable.
  ASSERT_EQ(folder->childCount(), 2);
  EXPECT_TRUE(folder->child(0)->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(folder->child(1)->flags().testFlag(Qt::ItemIsSelectable));
}

TEST(CurveTreeViewTest, ReturnsSortedSelectedLeafCurveNames) {
  PJ::CurveTreeView view;

  view.addCurve(QStringLiteral("root/b"));
  view.addCurve(QStringLiteral("root/a"));
  view.addCurve(QStringLiteral("z"));

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->text(0), QStringLiteral("root"));
  ASSERT_EQ(root->childCount(), 2);
  root->child(1)->setSelected(true);
  root->child(0)->setSelected(true);
  view.topLevelItem(1)->setSelected(true);

  EXPECT_EQ(toStdStrings(view.selectedCurveNames()), (std::vector<std::string>{"root/a", "root/b", "z"}));
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"root/a", "root/b", "z"}));
}

TEST(CurveTreeViewTest, PressingSelectedItemDoesNotCollapseMultiSelection) {
  TestCurveTreeView view;
  view.resize(240, 200);

  view.addCurve(QStringLiteral("root/b"));
  view.addCurve(QStringLiteral("root/a"));
  view.addCurve(QStringLiteral("z"));
  view.expandAll();
  view.show();
  QApplication::processEvents();

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 2);
  QTreeWidgetItem* first_leaf = root->child(0);
  QTreeWidgetItem* second_leaf = root->child(1);
  QTreeWidgetItem* top_leaf = view.topLevelItem(1);
  ASSERT_NE(first_leaf, nullptr);
  ASSERT_NE(second_leaf, nullptr);
  ASSERT_NE(top_leaf, nullptr);

  first_leaf->setSelected(true);
  second_leaf->setSelected(true);
  top_leaf->setSelected(true);

  const QPoint press_pos = view.visualItemRect(first_leaf).center();
  const QPointF local_pos(press_pos);
  const QPointF global_pos(view.viewport()->mapToGlobal(press_pos));
  QMouseEvent press_event(
      QEvent::MouseButtonPress, local_pos, local_pos, global_pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  view.mousePressEvent(&press_event);

  EXPECT_TRUE(first_leaf->isSelected());
  EXPECT_TRUE(second_leaf->isSelected());
  EXPECT_TRUE(top_leaf->isSelected());

  QMouseEvent release_event(
      QEvent::MouseButtonRelease, local_pos, local_pos, global_pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  view.mouseReleaseEvent(&release_event);

  EXPECT_TRUE(first_leaf->isSelected());
  EXPECT_TRUE(second_leaf->isSelected());
  EXPECT_TRUE(top_leaf->isSelected());
}

TEST(CurveTreeViewTest, DoubleClickOnDatasetTogglesOnlyDatasetExpansion) {
  PJ::CurveTreeView view;

  view.addCurve(QStringLiteral("root/branch/leaf_a"));
  view.addCurve(QStringLiteral("root/branch/leaf_b"));

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 1);
  QTreeWidgetItem* branch = root->child(0);
  ASSERT_NE(branch, nullptr);

  view.collapseAll();
  EXPECT_FALSE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());

  Q_EMIT view.itemDoubleClicked(root, 0);
  EXPECT_TRUE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());

  Q_EMIT view.itemDoubleClicked(root, 0);
  EXPECT_FALSE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());
}

TEST(CurveTreeViewTest, DoubleClickBelowDatasetTogglesWholeSubtreeExpansion) {
  PJ::CurveTreeView view;

  view.addCurve(QStringLiteral("root/branch/subbranch/leaf_a"));
  view.addCurve(QStringLiteral("root/branch/subbranch/leaf_b"));

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->childCount(), 1);
  QTreeWidgetItem* branch = root->child(0);
  ASSERT_NE(branch, nullptr);
  ASSERT_EQ(branch->childCount(), 1);
  QTreeWidgetItem* subbranch = branch->child(0);
  ASSERT_NE(subbranch, nullptr);

  view.collapseAll();
  EXPECT_FALSE(branch->isExpanded());
  EXPECT_FALSE(subbranch->isExpanded());

  Q_EMIT view.itemDoubleClicked(branch, 0);
  EXPECT_TRUE(branch->isExpanded());
  EXPECT_TRUE(subbranch->isExpanded());

  Q_EMIT view.itemDoubleClicked(branch, 0);
  EXPECT_FALSE(branch->isExpanded());
  EXPECT_FALSE(subbranch->isExpanded());
}

TEST(CurveTreeViewTest, ObjectTopicsUseTopicNodeWithoutEnteringCurveSelection) {
  PJ::CurveTreeView view;

  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("object:1"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/camera/image"),
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCurve(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("curve:1"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/camera/image"),
          .field = QStringLiteral("byte_count"),
      });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, QStringLiteral("camera"));
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* image = findChild(camera, QStringLiteral("image"));
  ASSERT_NE(image, nullptr);
  EXPECT_FALSE(image->font(0).italic());
  EXPECT_FALSE(image->icon(0).isNull());
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsDragEnabled));

  ASSERT_EQ(image->childCount(), 1);
  EXPECT_EQ(image->child(0)->text(0), QStringLiteral("byte_count"));
  EXPECT_TRUE(image->child(0)->flags().testFlag(Qt::ItemIsSelectable));

  image->setSelected(true);
  EXPECT_TRUE(view.selectedCurveNamesRecursive().empty());
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"object:1"}));

  image->child(0)->setSelected(true);
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"curve:1"}));
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"curve:1", "object:1"}));
}

TEST(CurveTreeViewTest, EncodesCatalogItemDragPayloads) {
  QMimeData mime_data;
  mime_data.setData(
      PJ::CurveTreeView::catalogItemsMimeType(),
      PJ::CurveTreeView::encodeCatalogKeys({QStringLiteral("object:1"), QStringLiteral("curve:1")}));

  const QStringList keys = PJ::CurveTreeView::decodeCatalogKeys(&mime_data);
  ASSERT_EQ(keys.size(), 2);
  EXPECT_EQ(keys[0], QStringLiteral("object:1"));
  EXPECT_EQ(keys[1], QStringLiteral("curve:1"));
}

// Regression: dragging a multi-selection onto an empty pane (which consumes the
// catalog-key payload) must add every selected curve, not just one. The catalog
// payload used to carry only the row under the cursor at press time.
TEST(CurveTreeViewTest, DragPayloadCarriesEverySelectedScalarCurve) {
  PJ::CurveTreeView view;
  view.addCurve(QStringLiteral("vehicle/speed"));
  view.addCurve(QStringLiteral("vehicle/rpm"));
  view.addCurve(QStringLiteral("vehicle/temp"));

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  for (const char* leaf : {"speed", "rpm", "temp"}) {
    QTreeWidgetItem* item = findChild(group, QString::fromLatin1(leaf));
    ASSERT_NE(item, nullptr) << leaf;
    item->setSelected(true);
  }

  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  ASSERT_NE(mime, nullptr);

  // Catalog payload — consumed when dropping on an empty pane / placeholder.
  const QStringList catalog_keys = PJ::CurveTreeView::decodeCatalogKeys(mime.get());
  EXPECT_EQ(catalog_keys.size(), 3);
  EXPECT_TRUE(catalog_keys.contains(QStringLiteral("vehicle/speed")));
  EXPECT_TRUE(catalog_keys.contains(QStringLiteral("vehicle/rpm")));
  EXPECT_TRUE(catalog_keys.contains(QStringLiteral("vehicle/temp")));

  // Curve-name payload — consumed when dropping on an existing plot.
  ASSERT_TRUE(mime->hasFormat(QStringLiteral("curveslist/add_curve")));
  QByteArray encoded = mime->data(QStringLiteral("curveslist/add_curve"));
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  int curve_count = 0;
  while (!stream.atEnd()) {
    QString name;
    stream >> name;
    if (!name.isEmpty()) {
      ++curve_count;
    }
  }
  EXPECT_EQ(curve_count, 3);
}

// Regression: the same defect on the object-topic side — a multi-selection of
// image/object topics (which carry no scalar curve names) must still ship every
// selected catalog key.
TEST(CurveTreeViewTest, DragPayloadCarriesEverySelectedObjectTopic) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("object:a"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/camera/front"),
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("object:b"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/camera/rear"),
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, QStringLiteral("camera"));
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* front = findChild(camera, QStringLiteral("front"));
  QTreeWidgetItem* rear = findChild(camera, QStringLiteral("rear"));
  ASSERT_NE(front, nullptr);
  ASSERT_NE(rear, nullptr);
  front->setSelected(true);
  rear->setSelected(true);

  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  ASSERT_NE(mime, nullptr);

  const QStringList catalog_keys = PJ::CurveTreeView::decodeCatalogKeys(mime.get());
  EXPECT_EQ(catalog_keys.size(), 2);
  EXPECT_TRUE(catalog_keys.contains(QStringLiteral("object:a")));
  EXPECT_TRUE(catalog_keys.contains(QStringLiteral("object:b")));
}

// The "Value" column keeps decimal points vertically aligned in a monospace
// right-aligned cell by formatting at a fixed precision then blanking trailing
// zeros (and a bare trailing dot) with spaces. Ported from PJ3.
TEST(CurveTreeViewTest, FormatScalarForColumnTrimsTrailingZerosToAlignDecimals) {
  EXPECT_EQ(PJ::formatScalarForColumn(1.2, 3), QStringLiteral("1.2") + QString(3, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(5.0, 3), QStringLiteral("5") + QString(5, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(-0.001, 3), QStringLiteral("-0.001 "));
  EXPECT_EQ(PJ::formatScalarForColumn(123.456, 3), QStringLiteral("123.456 "));
}

// refreshVisibleValues fills column 1 only for leaf rows, via the supplied
// provider keyed on each leaf's catalog key; group (non-leaf) rows stay empty.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsScalarLeavesAndSkipsGroups) {
  PJ::CurveTreeView view;
  view.addCurve(QStringLiteral("vehicle/speed"));
  view.addCurve(QStringLiteral("vehicle/rpm"));
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : QStringLiteral("42 "); });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->text(1), QString()) << "group node carries no value";
  ASSERT_EQ(group->childCount(), 2);
  EXPECT_EQ(group->child(0)->text(1), QStringLiteral("42 "));
  EXPECT_EQ(group->child(1)->text(1), QStringLiteral("42 "));
}

// When the value column is hidden the refresh is a no-op, so cells are not
// recomputed (and the per-row data reads are skipped entirely).
TEST(CurveTreeViewTest, RefreshVisibleValuesIsNoOpWhenValueColumnHidden) {
  PJ::CurveTreeView view;
  view.addCurve(QStringLiteral("vehicle/speed"));
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString&) { return QStringLiteral("42 "); });
  QTreeWidgetItem* leaf = view.topLevelItem(0)->child(0);
  ASSERT_NE(leaf, nullptr);
  ASSERT_EQ(leaf->text(1), QStringLiteral("42 "));

  view.setValuesColumnHidden(true);
  view.refreshVisibleValues([](const QString&) { return QStringLiteral("99 "); });
  EXPECT_EQ(leaf->text(1), QStringLiteral("42 ")) << "hidden value column must not refresh";
}

// Regression: expanding a collapsed group must fill the newly-revealed leaves
// from the retained provider, without the caller re-driving the refresh
// (previously a freshly-expanded row stayed blank until the tracker moved).
TEST(CurveTreeViewTest, RefreshVisibleValuesRefillsRowsRevealedByExpansion) {
  PJ::CurveTreeView view;
  view.addCurve(QStringLiteral("vehicle/speed"));
  view.setValuesColumnHidden(false);
  view.resize(400, 300);
  view.show();
  view.collapseAll();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : QStringLiteral("42 "); });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->childCount(), 1);
  QTreeWidgetItem* leaf = group->child(0);
  EXPECT_EQ(leaf->text(1), QString()) << "collapsed leaf is not filled yet";

  view.expandAll();
  QApplication::processEvents();  // flush the deferred re-apply scheduled by itemExpanded
  EXPECT_EQ(leaf->text(1), QStringLiteral("42 ")) << "expanding must fill the revealed leaf";
}

// Regression: scrolling down must fill the rows that scroll into view at the
// bottom of the viewport. A walk that stops at the first off-screen row (rather
// than culling each row independently) leaves the freshly-revealed rows blank.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsRowsRevealedByScrolling) {
  PJ::CurveTreeView view;
  for (int i = 0; i < 60; ++i) {
    view.addCurve(QStringLiteral("grp/c%1").arg(i, 2, 10, QChar('0')));
  }
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(300, 120);  // small viewport so the 60 rows overflow and can scroll
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : QStringLiteral("v "); });

  QScrollBar* scroll = view.verticalScrollBar();
  ASSERT_GT(scroll->maximum(), 0) << "content must overflow for the scroll case to be meaningful";
  scroll->setValue(scroll->maximum());  // scroll to the bottom
  QApplication::processEvents();        // flush the deferred refresh from valueChanged

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  QTreeWidgetItem* last_leaf = group->child(group->childCount() - 1);
  ASSERT_NE(last_leaf, nullptr);
  EXPECT_EQ(last_leaf->text(1), QStringLiteral("v ")) << "row scrolled into view must be filled";
}

// A value-only leaf (draggable=false, e.g. a string field) is shown and
// selectable but is NOT a drag source and never enters the drag payload.
TEST(CurveTreeViewTest, ValueOnlyLeafIsNotDraggableAndExcludedFromDragPayload) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("curve:num"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/diag"),
          .field = QStringLiteral("value"),
          .selectable = true,
          .draggable = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("curve:str"),
          .dataset = QStringLiteral("drive.mcap"),
          .topic = QStringLiteral("/diag"),
          .field = QStringLiteral("frame_id"),
          .selectable = true,
          .draggable = false,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* diag = findChild(dataset, QStringLiteral("diag"));
  ASSERT_NE(diag, nullptr);
  QTreeWidgetItem* num = findChild(diag, QStringLiteral("value"));
  QTreeWidgetItem* str = findChild(diag, QStringLiteral("frame_id"));
  ASSERT_NE(num, nullptr);
  ASSERT_NE(str, nullptr);

  // The string field is selectable (highlightable) but not a drag source.
  EXPECT_TRUE(str->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_FALSE(str->flags().testFlag(Qt::ItemIsDragEnabled));
  EXPECT_TRUE(num->flags().testFlag(Qt::ItemIsDragEnabled));

  // Selecting both yields a drag payload with only the numeric curve.
  num->setSelected(true);
  str->setSelected(true);
  EXPECT_EQ(toStdStrings(view.selectedCurveNamesRecursive()), (std::vector<std::string>{"curve:num"}));
  EXPECT_EQ(toStdStrings(view.selectedCatalogKeysRecursive()), (std::vector<std::string>{"curve:num"}));

  // A string-only selection produces no draggable payload at all.
  num->setSelected(false);
  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  EXPECT_EQ(mime, nullptr) << "a string-only selection must not start a drag";
}

TEST(CurveTreeViewTest, ExpandedGroupPathsSurviveRebuild) {
  // A full rebuild (clearCurves + re-add) runs on every catalog removal —
  // routine under demand-driven streaming (placeholder supersede). The
  // expanded-state snapshot must restore what still exists, skip what
  // vanished, and leave everything else collapsed as it was.
  PJ::CurveTreeView view;
  const auto add_all = [&view]() {
    view.addCatalogItems({
        PJ::CurveTreeView::CurvePath{
            .key = QStringLiteral("k1"),
            .dataset = QStringLiteral("robot"),
            .topic = QStringLiteral("imu"),
            .field = QStringLiteral("x"),
        },
        PJ::CurveTreeView::CurvePath{
            .key = QStringLiteral("k2"),
            .dataset = QStringLiteral("robot"),
            .topic = QStringLiteral("odom"),
            .field = QStringLiteral("x"),
        },
    });
  };
  add_all();

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  robot->setExpanded(true);
  QTreeWidgetItem* imu = findChild(robot, QStringLiteral("imu"));
  ASSERT_NE(imu, nullptr);
  imu->setExpanded(true);
  QTreeWidgetItem* odom = findChild(robot, QStringLiteral("odom"));
  ASSERT_NE(odom, nullptr);
  ASSERT_FALSE(odom->isExpanded());

  const QStringList expanded = view.expandedGroupPaths();

  view.clearCurves();
  add_all();
  QTreeWidgetItem* rebuilt_robot = view.topLevelItem(0);
  ASSERT_NE(rebuilt_robot, nullptr);
  ASSERT_FALSE(rebuilt_robot->isExpanded());

  view.restoreExpandedGroupPaths(expanded);

  EXPECT_TRUE(rebuilt_robot->isExpanded());
  QTreeWidgetItem* rebuilt_imu = findChild(rebuilt_robot, QStringLiteral("imu"));
  ASSERT_NE(rebuilt_imu, nullptr);
  EXPECT_TRUE(rebuilt_imu->isExpanded());
  QTreeWidgetItem* rebuilt_odom = findChild(rebuilt_robot, QStringLiteral("odom"));
  ASSERT_NE(rebuilt_odom, nullptr);
  EXPECT_FALSE(rebuilt_odom->isExpanded());
}

TEST(CurveTreeViewTest, SetUnsubscribedKeysReplacesTheFullSet) {
  PJ::CurveTreeView view;
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("k1"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("imu"),
          .field = QStringLiteral("x"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("k2"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("odom"),
          .field = QStringLiteral("x"),
      },
  });

  EXPECT_FALSE(view.isKeyUnsubscribed(QStringLiteral("k1")));
  EXPECT_FALSE(view.isKeyUnsubscribed(QStringLiteral("k2")));

  view.setUnsubscribedKeys({QStringLiteral("k1")});
  EXPECT_TRUE(view.isKeyUnsubscribed(QStringLiteral("k1")));
  EXPECT_FALSE(view.isKeyUnsubscribed(QStringLiteral("k2")));

  // Full-set replace: re-subscribing k1 and unsubscribing k2 in one call
  // flips both, not just adds k2.
  view.setUnsubscribedKeys({QStringLiteral("k2")});
  EXPECT_FALSE(view.isKeyUnsubscribed(QStringLiteral("k1")));
  EXPECT_TRUE(view.isKeyUnsubscribed(QStringLiteral("k2")));

  EXPECT_FALSE(view.isKeyUnsubscribed(QStringLiteral("no-such-key")));
}

TEST(CurveTreeViewTest, IsUnsubscribedSeedsFromCurvePathAtConstruction) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("k1"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("imu"),
          .field = QStringLiteral("x"),
          .is_unsubscribed = true,
      });
  EXPECT_TRUE(view.isKeyUnsubscribed(QStringLiteral("k1")));
}

namespace {
// First direct child of `parent` whose Name-column text is `text`, or nullptr.
QTreeWidgetItem* peekChildByText(QTreeWidgetItem* parent, const QString& text) {
  for (int i = 0; i < parent->childCount(); ++i) {
    if (parent->child(i)->text(0) == text) {
      return parent->child(i);
    }
  }
  return nullptr;
}
}  // namespace

TEST(CurveTreeViewTest, DoubleClickEmitsPeekOnlyForScalarPlaceholderLeaf) {
  PJ::CurveTreeView view;  // default hierarchical view
  // Scalar-shaped placeholder: a draggable leaf with no field breakdown yet.
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("scalar-key"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("/speed"),
          .field = QString(),
          .selectable = true,
          .is_placeholder = true,
      });
  // Object-shaped placeholder: a non-selectable 3D-object terminal (also a
  // childless node, but NOT peek-eligible).
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("object-key"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("/points"),
          .field = QString(),
          .selectable = false,
          .is_3d_object_topic = true,
          .is_placeholder = true,
      });
  // Real (subscribed) scalar leaf: not a placeholder.
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("real-key"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("/temp"),
          .field = QString(),
          .selectable = true,
          .is_placeholder = false,
      });

  QStringList captured;
  QObject::connect(
      &view, &PJ::CurveTreeView::placeholderPeekRequested, [&](const QString& key) { captured.push_back(key); });

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* scalar_leaf = peekChildByText(root, QStringLiteral("speed"));
  QTreeWidgetItem* object_terminal = peekChildByText(root, QStringLiteral("points"));
  QTreeWidgetItem* real_leaf = peekChildByText(root, QStringLiteral("temp"));
  ASSERT_NE(scalar_leaf, nullptr);
  ASSERT_NE(object_terminal, nullptr);
  ASSERT_NE(real_leaf, nullptr);

  Q_EMIT view.itemDoubleClicked(object_terminal, 0);
  Q_EMIT view.itemDoubleClicked(real_leaf, 0);
  EXPECT_TRUE(captured.isEmpty()) << "object placeholder and real leaf must not emit a peek";

  Q_EMIT view.itemDoubleClicked(scalar_leaf, 0);
  EXPECT_EQ(captured, QStringList{QStringLiteral("scalar-key")});
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresOnceThenRespectsManualCollapse) {
  PJ::CurveTreeView view;  // default hierarchical view

  // Arm the intent for the topic's tree-path while it is still a placeholder leaf.
  view.requestExpansionWhenPromoted(QStringLiteral("robot/imu/data"));

  // Promotion: the placeholder leaf is replaced by real field leaves under the
  // topic, so "robot/imu/data" becomes a group node.
  const auto promote = [&view]() {
    view.clearCurves();
    view.addCatalogItems(
        {PJ::CurveTreeView::CurvePath{
             .key = QStringLiteral("f1"),
             .dataset = QStringLiteral("robot"),
             .topic = QStringLiteral("/imu/data"),
             .field = QStringLiteral("angular_velocity.z"),
         },
         PJ::CurveTreeView::CurvePath{
             .key = QStringLiteral("f2"),
             .dataset = QStringLiteral("robot"),
             .topic = QStringLiteral("/imu/data"),
             .field = QStringLiteral("orientation.w"),
         }});
  };

  promote();
  QTreeWidgetItem* imu = peekChildByText(view.topLevelItem(0), QStringLiteral("imu"));
  ASSERT_NE(imu, nullptr);
  QTreeWidgetItem* data_group = peekChildByText(imu, QStringLiteral("data"));
  ASSERT_NE(data_group, nullptr);
  EXPECT_TRUE(data_group->isExpanded()) << "the promoted topic group auto-expands once";

  // One-shot: a manual collapse must survive the next rebuild (the intent was
  // already consumed).
  data_group->setExpanded(false);
  promote();
  QTreeWidgetItem* imu2 = peekChildByText(view.topLevelItem(0), QStringLiteral("imu"));
  ASSERT_NE(imu2, nullptr);
  QTreeWidgetItem* data_group2 = peekChildByText(imu2, QStringLiteral("data"));
  ASSERT_NE(data_group2, nullptr);
  EXPECT_FALSE(data_group2->isExpanded()) << "auto-expand must not re-fire after the one-shot intent is consumed";
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresInShowTopicsView) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);  // the app's default view
  view.requestExpansionWhenPromoted(QStringLiteral("robot/imu/data"));

  view.addCatalogItems(
      {PJ::CurveTreeView::CurvePath{
           .key = QStringLiteral("f1"),
           .dataset = QStringLiteral("robot"),
           .topic = QStringLiteral("/imu/data"),
           .field = QStringLiteral("angular_velocity.z"),
       },
       PJ::CurveTreeView::CurvePath{
           .key = QStringLiteral("f2"),
           .dataset = QStringLiteral("robot"),
           .topic = QStringLiteral("/imu/data"),
           .field = QStringLiteral("orientation.w"),
       }});

  // In show-topics view the topic is a single verbatim node "/imu/data" whose
  // text diverges from the normalized search path — the search-role keying still
  // resolves and expands it.
  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* topic = peekChildByText(root, QStringLiteral("/imu/data"));
  ASSERT_NE(topic, nullptr);
  EXPECT_TRUE(topic->isExpanded());
}

TEST(CurveTreeViewTest, CatalogKeysUnderCollectsSelfAndDescendants) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("k_x"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("/imu/data"),
          .field = QStringLiteral("x"),
      },
      PJ::CurveTreeView::CurvePath{
          .key = QStringLiteral("k_y"),
          .dataset = QStringLiteral("robot"),
          .topic = QStringLiteral("/imu/data"),
          .field = QStringLiteral("y"),
      },
  });

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  QTreeWidgetItem* topic = findChild(robot, QStringLiteral("/imu/data"));
  ASSERT_NE(topic, nullptr);
  ASSERT_TRUE(PJ::CurveTreeView::catalogKeyOf(topic).isEmpty()) << "a promoted topic group carries no key itself";

  QStringList keys = PJ::CurveTreeView::catalogKeysUnder(topic);
  keys.sort();
  EXPECT_EQ(keys, (QStringList{QStringLiteral("k_x"), QStringLiteral("k_y")}));

  // A keyed leaf reports just itself.
  QTreeWidgetItem* leaf_x = findChild(topic, QStringLiteral("x"));
  ASSERT_NE(leaf_x, nullptr);
  EXPECT_EQ(PJ::CurveTreeView::catalogKeysUnder(leaf_x), (QStringList{QStringLiteral("k_x")}));
}

TEST(CurveTreeViewTest, ForcedTopicMarksLandOnTheTopicNodeAndSurviveRebuild) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  const auto add_all = [&view]() {
    view.addCatalogItems({
        PJ::CurveTreeView::CurvePath{
            .key = QStringLiteral("k_x"),
            .dataset = QStringLiteral("robot"),
            .topic = QStringLiteral("/imu/data"),
            .field = QStringLiteral("x"),
        },
        PJ::CurveTreeView::CurvePath{
            .key = QStringLiteral("k_pc"),
            .dataset = QStringLiteral("robot"),
            .topic = QStringLiteral("/points"),
            .field = {},
            .selectable = false,  // object-topic terminal — IS the topic row
        },
    });
  };
  add_all();

  const QString imu_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{
          .key = {}, .dataset = QStringLiteral("robot"), .topic = QStringLiteral("/imu/data"), .field = {}});
  const QString pc_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{
          .key = {}, .dataset = QStringLiteral("robot"), .topic = QStringLiteral("/points"), .field = {}});

  view.setForcedTopicPaths({imu_path, pc_path});
  EXPECT_TRUE(view.isTopicPathForced(imu_path)) << "promoted scalar topic: mark on the keyless GROUP node";
  EXPECT_TRUE(view.isTopicPathForced(pc_path)) << "object terminal: mark on the topic row itself";

  // The set is retained: a rebuild (clear + re-add) re-stamps the marks.
  view.clearCurves();
  add_all();
  EXPECT_TRUE(view.isTopicPathForced(imu_path));

  // Full-set replace: unforcing clears the mark.
  view.setForcedTopicPaths({});
  EXPECT_FALSE(view.isTopicPathForced(imu_path));
  EXPECT_FALSE(view.isTopicPathForced(pc_path));
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
