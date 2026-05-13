#include <gtest/gtest.h>

#include <QApplication>
#include <QMouseEvent>
#include <QTreeWidgetItem>
#include <QtGlobal>
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

TEST(CurveTreeViewTest, DoubleClickTogglesWholeSubtreeExpansion) {
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
  EXPECT_TRUE(branch->isExpanded());

  Q_EMIT view.itemDoubleClicked(root, 0);
  EXPECT_FALSE(root->isExpanded());
  EXPECT_FALSE(branch->isExpanded());
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
