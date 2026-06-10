// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QMimeData>
#include <QMouseEvent>
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

TEST(CurveTreeViewTest, UsesPj3StyleRowSelectionAndLeafOnlyGroups) {
  PJ::CurveTreeView view;

  view.addCurve(QStringLiteral("root/b"));
  view.addCurve(QStringLiteral("root/a"));

  ASSERT_EQ(view.selectionMode(), QAbstractItemView::ExtendedSelection);
  ASSERT_EQ(view.selectionBehavior(), QAbstractItemView::SelectRows);

  QTreeWidgetItem* root = view.topLevelItem(0);
  ASSERT_NE(root, nullptr);
  EXPECT_FALSE(root->flags().testFlag(Qt::ItemIsSelectable));
  ASSERT_EQ(root->childCount(), 2);
  EXPECT_TRUE(root->child(0)->flags().testFlag(Qt::ItemIsSelectable));
  EXPECT_TRUE(root->child(1)->flags().testFlag(Qt::ItemIsSelectable));
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

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
