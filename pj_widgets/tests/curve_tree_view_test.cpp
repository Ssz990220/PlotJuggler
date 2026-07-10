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
using namespace Qt::StringLiterals;

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

  view.addCurve(u"gamma/zeta"_s);
  view.addCurve(u"alpha/delta"_s);
  view.addCurve(u"beta/root"_s);
  view.addCurve(u"alpha/charlie"_s);
  view.addCurve(u"alpha/bravo"_s);

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), u"alpha"_s);
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

TEST(CurveTreeViewTest, BatchedCatalogInsertSortsTopLevelGroupsAndChildren) {
  PJ::CurveTreeView view;

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"gamma/zeta"_s,
          .dataset = u"gamma"_s,
          .topic = {},
          .field = u"zeta"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/delta"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"delta"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"beta/root"_s,
          .dataset = u"beta"_s,
          .topic = {},
          .field = u"root"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/charlie"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"charlie"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"alpha/bravo"_s,
          .dataset = u"alpha"_s,
          .topic = {},
          .field = u"bravo"_s,
      },
  });

  EXPECT_EQ(topLevelNames(view), (std::vector<std::string>{"alpha", "beta", "gamma"}));

  ASSERT_EQ(view.topLevelItemCount(), 3);
  QTreeWidgetItem* alpha = view.topLevelItem(0);
  ASSERT_EQ(alpha->text(0), u"alpha"_s);
  EXPECT_EQ(childNames(alpha), (std::vector<std::string>{"bravo", "charlie", "delta"}));
}

// A filter typed BEFORE data is loaded must apply to the rows that arrive later
// (the catalog-insert path), not just to rows already in the tree.
TEST(CurveTreeViewTest, FilterAppliesToRowsInsertedAfterItWasSet) {
  PJ::CurveTreeView view;

  view.applyFilter(u"imu"_s);

  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"veh/imu/x"_s,
          .dataset = u"veh"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"veh/gps/lat"_s,
          .dataset = u"veh"_s,
          .topic = u"gps"_s,
          .field = u"lat"_s,
      },
  });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* veh = view.topLevelItem(0);
  EXPECT_FALSE(veh->isHidden());

  QTreeWidgetItem* imu = findChild(veh, u"imu"_s);
  ASSERT_NE(imu, nullptr);
  EXPECT_FALSE(imu->isHidden());
  EXPECT_FALSE(findChild(imu, u"x"_s)->isHidden());

  QTreeWidgetItem* gps = findChild(veh, u"gps"_s);
  ASSERT_NE(gps, nullptr);
  EXPECT_TRUE(gps->isHidden());
  EXPECT_TRUE(findChild(gps, u"lat"_s)->isHidden());
}

TEST(CurveTreeViewTest, TopLevelGroupsSelectableIntermediateGroupsLeafOnly) {
  PJ::CurveTreeView view;

  // Two-level path: "dataset" (top-level group) / "folder" (intermediate) / leaf.
  view.addCurve(u"dataset/folder/b"_s);
  view.addCurve(u"dataset/folder/a"_s);

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

  view.addCurve(u"root/b"_s);
  view.addCurve(u"root/a"_s);
  view.addCurve(u"z"_s);

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  ASSERT_EQ(root->text(0), u"root"_s);
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

  view.addCurve(u"root/b"_s);
  view.addCurve(u"root/a"_s);
  view.addCurve(u"z"_s);
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

  view.addCurve(u"root/branch/leaf_a"_s);
  view.addCurve(u"root/branch/leaf_b"_s);

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

  view.addCurve(u"root/branch/subbranch/leaf_a"_s);
  view.addCurve(u"root/branch/subbranch/leaf_b"_s);

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
          .key = u"object:1"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/image"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCurve(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:1"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/image"_s,
          .field = u"byte_count"_s,
      });

  ASSERT_EQ(view.topLevelItemCount(), 1);
  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, u"camera"_s);
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* image = findChild(camera, u"image"_s);
  ASSERT_NE(image, nullptr);
  EXPECT_FALSE(image->font(0).italic());
  EXPECT_FALSE(image->icon(0).isNull());
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(image->flags().testFlag(Qt::ItemIsDragEnabled));

  ASSERT_EQ(image->childCount(), 1);
  EXPECT_EQ(image->child(0)->text(0), u"byte_count"_s);
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
      PJ::CurveTreeView::catalogItemsMimeType(), PJ::CurveTreeView::encodeCatalogKeys({u"object:1"_s, u"curve:1"_s}));

  const QStringList keys = PJ::CurveTreeView::decodeCatalogKeys(&mime_data);
  ASSERT_EQ(keys.size(), 2);
  EXPECT_EQ(keys[0], u"object:1"_s);
  EXPECT_EQ(keys[1], u"curve:1"_s);
}

// Regression: dragging a multi-selection onto an empty pane (which consumes the
// catalog-key payload) must add every selected curve, not just one. The catalog
// payload used to carry only the row under the cursor at press time.
TEST(CurveTreeViewTest, DragPayloadCarriesEverySelectedScalarCurve) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.addCurve(u"vehicle/rpm"_s);
  view.addCurve(u"vehicle/temp"_s);

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
  EXPECT_TRUE(catalog_keys.contains(u"vehicle/speed"_s));
  EXPECT_TRUE(catalog_keys.contains(u"vehicle/rpm"_s));
  EXPECT_TRUE(catalog_keys.contains(u"vehicle/temp"_s));

  // Curve-name payload — consumed when dropping on an existing plot.
  ASSERT_TRUE(mime->hasFormat(u"curveslist/add_curve"_s));
  QByteArray encoded = mime->data(u"curveslist/add_curve"_s);
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
          .key = u"object:a"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/front"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object:b"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/camera/rear"_s,
          .field = {},
          .selectable = false,
          .is_image_topic = true,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* camera = findChild(dataset, u"camera"_s);
  ASSERT_NE(camera, nullptr);
  QTreeWidgetItem* front = findChild(camera, u"front"_s);
  QTreeWidgetItem* rear = findChild(camera, u"rear"_s);
  ASSERT_NE(front, nullptr);
  ASSERT_NE(rear, nullptr);
  front->setSelected(true);
  rear->setSelected(true);

  std::unique_ptr<QMimeData> mime(view.createDragMimeData(Qt::LeftButton));
  ASSERT_NE(mime, nullptr);

  const QStringList catalog_keys = PJ::CurveTreeView::decodeCatalogKeys(mime.get());
  EXPECT_EQ(catalog_keys.size(), 2);
  EXPECT_TRUE(catalog_keys.contains(u"object:a"_s));
  EXPECT_TRUE(catalog_keys.contains(u"object:b"_s));
}

// The "Value" column keeps decimal points vertically aligned in a monospace
// right-aligned cell by formatting at a fixed precision then blanking trailing
// zeros (and a bare trailing dot) with spaces. Ported from PJ3.
TEST(CurveTreeViewTest, FormatScalarForColumnTrimsTrailingZerosToAlignDecimals) {
  EXPECT_EQ(PJ::formatScalarForColumn(1.2, 3), u"1.2"_s + QString(3, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(5.0, 3), u"5"_s + QString(5, QChar(' ')));
  EXPECT_EQ(PJ::formatScalarForColumn(-0.001, 3), u"-0.001 "_s);
  EXPECT_EQ(PJ::formatScalarForColumn(123.456, 3), u"123.456 "_s);
}

// refreshVisibleValues fills column 1 only for leaf rows, via the supplied
// provider keyed on each leaf's catalog key; group (non-leaf) rows stay empty.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsScalarLeavesAndSkipsGroups) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.addCurve(u"vehicle/rpm"_s);
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"42 "_s; });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(group->text(1), QString()) << "group node carries no value";
  ASSERT_EQ(group->childCount(), 2);
  EXPECT_EQ(group->child(0)->text(1), u"42 "_s);
  EXPECT_EQ(group->child(1)->text(1), u"42 "_s);
}

// When the value column is hidden the refresh is a no-op, so cells are not
// recomputed (and the per-row data reads are skipped entirely).
TEST(CurveTreeViewTest, RefreshVisibleValuesIsNoOpWhenValueColumnHidden) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(400, 300);
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString&) { return u"42 "_s; });
  QTreeWidgetItem* leaf = view.topLevelItem(0)->child(0);
  ASSERT_NE(leaf, nullptr);
  ASSERT_EQ(leaf->text(1), u"42 "_s);

  view.setValuesColumnHidden(true);
  view.refreshVisibleValues([](const QString&) { return u"99 "_s; });
  EXPECT_EQ(leaf->text(1), u"42 "_s) << "hidden value column must not refresh";
}

// Regression: expanding a collapsed group must fill the newly-revealed leaves
// from the retained provider, without the caller re-driving the refresh
// (previously a freshly-expanded row stayed blank until the tracker moved).
TEST(CurveTreeViewTest, RefreshVisibleValuesRefillsRowsRevealedByExpansion) {
  PJ::CurveTreeView view;
  view.addCurve(u"vehicle/speed"_s);
  view.setValuesColumnHidden(false);
  view.resize(400, 300);
  view.show();
  view.collapseAll();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"42 "_s; });

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  ASSERT_EQ(group->childCount(), 1);
  QTreeWidgetItem* leaf = group->child(0);
  EXPECT_EQ(leaf->text(1), QString()) << "collapsed leaf is not filled yet";

  view.expandAll();
  QApplication::processEvents();  // flush the deferred re-apply scheduled by itemExpanded
  EXPECT_EQ(leaf->text(1), u"42 "_s) << "expanding must fill the revealed leaf";
}

// Regression: scrolling down must fill the rows that scroll into view at the
// bottom of the viewport. A walk that stops at the first off-screen row (rather
// than culling each row independently) leaves the freshly-revealed rows blank.
TEST(CurveTreeViewTest, RefreshVisibleValuesFillsRowsRevealedByScrolling) {
  PJ::CurveTreeView view;
  for (int i = 0; i < 60; ++i) {
    view.addCurve(u"grp/c%1"_s.arg(i, 2, 10, QChar('0')));
  }
  view.setValuesColumnHidden(false);
  view.expandAll();
  view.resize(300, 120);  // small viewport so the 60 rows overflow and can scroll
  view.show();
  QApplication::processEvents();

  view.refreshVisibleValues([](const QString& key) { return key.isEmpty() ? QString() : u"v "_s; });

  QScrollBar* scroll = view.verticalScrollBar();
  ASSERT_GT(scroll->maximum(), 0) << "content must overflow for the scroll case to be meaningful";
  scroll->setValue(scroll->maximum());  // scroll to the bottom
  QApplication::processEvents();        // flush the deferred refresh from valueChanged

  QTreeWidgetItem* group = view.topLevelItem(0);
  ASSERT_NE(group, nullptr);
  QTreeWidgetItem* last_leaf = group->child(group->childCount() - 1);
  ASSERT_NE(last_leaf, nullptr);
  EXPECT_EQ(last_leaf->text(1), u"v "_s) << "row scrolled into view must be filled";
}

// A value-only leaf (draggable=false, e.g. a string field) is shown and
// selectable but is NOT a drag source and never enters the drag payload.
TEST(CurveTreeViewTest, ValueOnlyLeafIsNotDraggableAndExcludedFromDragPayload) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:num"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/diag"_s,
          .field = u"value"_s,
          .selectable = true,
          .draggable = true,
      });
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"curve:str"_s,
          .dataset = u"drive.mcap"_s,
          .topic = u"/diag"_s,
          .field = u"frame_id"_s,
          .selectable = true,
          .draggable = false,
      });

  QTreeWidgetItem* dataset = view.topLevelItem(0);
  ASSERT_NE(dataset, nullptr);
  QTreeWidgetItem* diag = findChild(dataset, u"diag"_s);
  ASSERT_NE(diag, nullptr);
  QTreeWidgetItem* num = findChild(diag, u"value"_s);
  QTreeWidgetItem* str = findChild(diag, u"frame_id"_s);
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
            .key = u"k1"_s,
            .dataset = u"robot"_s,
            .topic = u"imu"_s,
            .field = u"x"_s,
        },
        PJ::CurveTreeView::CurvePath{
            .key = u"k2"_s,
            .dataset = u"robot"_s,
            .topic = u"odom"_s,
            .field = u"x"_s,
        },
    });
  };
  add_all();

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  robot->setExpanded(true);
  QTreeWidgetItem* imu = findChild(robot, u"imu"_s);
  ASSERT_NE(imu, nullptr);
  imu->setExpanded(true);
  QTreeWidgetItem* odom = findChild(robot, u"odom"_s);
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
  QTreeWidgetItem* rebuilt_imu = findChild(rebuilt_robot, u"imu"_s);
  ASSERT_NE(rebuilt_imu, nullptr);
  EXPECT_TRUE(rebuilt_imu->isExpanded());
  QTreeWidgetItem* rebuilt_odom = findChild(rebuilt_robot, u"odom"_s);
  ASSERT_NE(rebuilt_odom, nullptr);
  EXPECT_FALSE(rebuilt_odom->isExpanded());
}

TEST(CurveTreeViewTest, SetUnsubscribedKeysReplacesTheFullSet) {
  PJ::CurveTreeView view;
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"k1"_s,
          .dataset = u"robot"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"k2"_s,
          .dataset = u"robot"_s,
          .topic = u"odom"_s,
          .field = u"x"_s,
      },
  });

  EXPECT_FALSE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k2"_s));

  view.setUnsubscribedKeys({u"k1"_s});
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k2"_s));

  // Full-set replace: re-subscribing k1 and unsubscribing k2 in one call
  // flips both, not just adds k2.
  view.setUnsubscribedKeys({u"k2"_s});
  EXPECT_FALSE(view.isKeyUnsubscribed(u"k1"_s));
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k2"_s));

  EXPECT_FALSE(view.isKeyUnsubscribed(u"no-such-key"_s));
}

TEST(CurveTreeViewTest, IsUnsubscribedSeedsFromCurvePathAtConstruction) {
  PJ::CurveTreeView view;
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"k1"_s,
          .dataset = u"robot"_s,
          .topic = u"imu"_s,
          .field = u"x"_s,
          .is_unsubscribed = true,
      });
  EXPECT_TRUE(view.isKeyUnsubscribed(u"k1"_s));
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
          .key = u"scalar-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/speed"_s,
          .field = QString(),
          .selectable = true,
          .is_placeholder = true,
      });
  // Object-shaped placeholder: a non-selectable 3D-object terminal (also a
  // childless node, but NOT peek-eligible).
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"object-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/points"_s,
          .field = QString(),
          .selectable = false,
          .is_3d_object_topic = true,
          .is_placeholder = true,
      });
  // Real (subscribed) scalar leaf: not a placeholder.
  view.addCatalogItem(
      PJ::CurveTreeView::CurvePath{
          .key = u"real-key"_s,
          .dataset = u"robot"_s,
          .topic = u"/temp"_s,
          .field = QString(),
          .selectable = true,
          .is_placeholder = false,
      });

  QStringList captured;
  QObject::connect(
      &view, &PJ::CurveTreeView::placeholderPeekRequested, [&](const QString& key) { captured.push_back(key); });

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* scalar_leaf = peekChildByText(root, u"speed"_s);
  QTreeWidgetItem* object_terminal = peekChildByText(root, u"points"_s);
  QTreeWidgetItem* real_leaf = peekChildByText(root, u"temp"_s);
  ASSERT_NE(scalar_leaf, nullptr);
  ASSERT_NE(object_terminal, nullptr);
  ASSERT_NE(real_leaf, nullptr);

  Q_EMIT view.itemDoubleClicked(object_terminal, 0);
  Q_EMIT view.itemDoubleClicked(real_leaf, 0);
  EXPECT_TRUE(captured.isEmpty()) << "object placeholder and real leaf must not emit a peek";

  Q_EMIT view.itemDoubleClicked(scalar_leaf, 0);
  EXPECT_EQ(captured, QStringList{u"scalar-key"_s});
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresOnceThenRespectsManualCollapse) {
  PJ::CurveTreeView view;  // default hierarchical view

  // Arm the intent for the topic's tree-path while it is still a placeholder leaf.
  view.requestExpansionWhenPromoted(u"robot/imu/data"_s);

  // Promotion: the placeholder leaf is replaced by real field leaves under the
  // topic, so "robot/imu/data" becomes a group node.
  const auto promote = [&view]() {
    view.clearCurves();
    view.addCatalogItems(
        {PJ::CurveTreeView::CurvePath{
             .key = u"f1"_s,
             .dataset = u"robot"_s,
             .topic = u"/imu/data"_s,
             .field = u"angular_velocity.z"_s,
         },
         PJ::CurveTreeView::CurvePath{
             .key = u"f2"_s,
             .dataset = u"robot"_s,
             .topic = u"/imu/data"_s,
             .field = u"orientation.w"_s,
         }});
  };

  promote();
  QTreeWidgetItem* imu = peekChildByText(view.topLevelItem(0), u"imu"_s);
  ASSERT_NE(imu, nullptr);
  QTreeWidgetItem* data_group = peekChildByText(imu, u"data"_s);
  ASSERT_NE(data_group, nullptr);
  EXPECT_TRUE(data_group->isExpanded()) << "the promoted topic group auto-expands once";

  // One-shot: a manual collapse must survive the next rebuild (the intent was
  // already consumed).
  data_group->setExpanded(false);
  promote();
  QTreeWidgetItem* imu2 = peekChildByText(view.topLevelItem(0), u"imu"_s);
  ASSERT_NE(imu2, nullptr);
  QTreeWidgetItem* data_group2 = peekChildByText(imu2, u"data"_s);
  ASSERT_NE(data_group2, nullptr);
  EXPECT_FALSE(data_group2->isExpanded()) << "auto-expand must not re-fire after the one-shot intent is consumed";
}

TEST(CurveTreeViewTest, RequestExpansionWhenPromotedFiresInShowTopicsView) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);  // the app's default view
  view.requestExpansionWhenPromoted(u"robot/imu/data"_s);

  view.addCatalogItems(
      {PJ::CurveTreeView::CurvePath{
           .key = u"f1"_s,
           .dataset = u"robot"_s,
           .topic = u"/imu/data"_s,
           .field = u"angular_velocity.z"_s,
       },
       PJ::CurveTreeView::CurvePath{
           .key = u"f2"_s,
           .dataset = u"robot"_s,
           .topic = u"/imu/data"_s,
           .field = u"orientation.w"_s,
       }});

  // In show-topics view the topic is a single verbatim node "/imu/data" whose
  // text diverges from the normalized search path — the search-role keying still
  // resolves and expands it.
  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  QTreeWidgetItem* topic = peekChildByText(root, u"/imu/data"_s);
  ASSERT_NE(topic, nullptr);
  EXPECT_TRUE(topic->isExpanded());
}

TEST(CurveTreeViewTest, CatalogKeysUnderCollectsSelfAndDescendants) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  view.addCatalogItems({
      PJ::CurveTreeView::CurvePath{
          .key = u"k_x"_s,
          .dataset = u"robot"_s,
          .topic = u"/imu/data"_s,
          .field = u"x"_s,
      },
      PJ::CurveTreeView::CurvePath{
          .key = u"k_y"_s,
          .dataset = u"robot"_s,
          .topic = u"/imu/data"_s,
          .field = u"y"_s,
      },
  });

  QTreeWidgetItem* robot = view.topLevelItem(0);
  ASSERT_NE(robot, nullptr);
  QTreeWidgetItem* topic = findChild(robot, u"/imu/data"_s);
  ASSERT_NE(topic, nullptr);
  ASSERT_TRUE(PJ::CurveTreeView::catalogKeyOf(topic).isEmpty()) << "a promoted topic group carries no key itself";

  QStringList keys = PJ::CurveTreeView::catalogKeysUnder(topic);
  keys.sort();
  EXPECT_EQ(keys, (QStringList{u"k_x"_s, u"k_y"_s}));

  // A keyed leaf reports just itself.
  QTreeWidgetItem* leaf_x = findChild(topic, u"x"_s);
  ASSERT_NE(leaf_x, nullptr);
  EXPECT_EQ(PJ::CurveTreeView::catalogKeysUnder(leaf_x), (QStringList{u"k_x"_s}));
}

TEST(CurveTreeViewTest, ForcedTopicMarksLandOnTheTopicNodeAndSurviveRebuild) {
  PJ::CurveTreeView view;
  view.setViewMode(PJ::CurveTreeView::ViewMode::kShowTopics);
  const auto add_all = [&view]() {
    view.addCatalogItems({
        PJ::CurveTreeView::CurvePath{
            .key = u"k_x"_s,
            .dataset = u"robot"_s,
            .topic = u"/imu/data"_s,
            .field = u"x"_s,
        },
        PJ::CurveTreeView::CurvePath{
            .key = u"k_pc"_s,
            .dataset = u"robot"_s,
            .topic = u"/points"_s,
            .field = {},
            .selectable = false,  // object-topic terminal — IS the topic row
        },
    });
  };
  add_all();

  const QString imu_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{.key = {}, .dataset = u"robot"_s, .topic = u"/imu/data"_s, .field = {}});
  const QString pc_path = PJ::CurveTreeView::treePathFromCurvePath(
      PJ::CurveTreeView::CurvePath{.key = {}, .dataset = u"robot"_s, .topic = u"/points"_s, .field = {}});

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
