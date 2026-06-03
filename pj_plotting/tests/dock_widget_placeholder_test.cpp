// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMetaObject>
#include <QMimeData>
#include <QStringList>
#include <QToolButton>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotDocker.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/IDataWidget.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/VisualizationPlaceholderWidget.h"

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  const auto committed_topics = session.commitChunks(writer.flushAll());
  EXPECT_FALSE(committed_topics.empty());
  return handle_or->topic_id;
}

class TestPlaceholderWidget : public PJ::VisualizationPlaceholderWidget {
 public:
  using PJ::VisualizationPlaceholderWidget::VisualizationPlaceholderWidget;

  void sendDragEnter(QDragEnterEvent* event) {
    dragEnterEvent(event);
  }

  void sendDragMove(QDragMoveEvent* event) {
    dragMoveEvent(event);
  }

  void sendDrop(QDropEvent* event) {
    dropEvent(event);
  }

  bool sendFilteredEvent(QObject* watched, QEvent* event) {
    return eventFilter(watched, event);
  }
};

class FakeObjectWidget : public QWidget, public PJ::IDataWidget {
 public:
  using QWidget::QWidget;

  QWidget* widget() override {
    return this;
  }

  void onTrackerTime(double /*time*/) override {}
};

QAction* findActionByText(QObject* parent, const QString& text) {
  for (auto* action : parent->findChildren<QAction*>()) {
    QString action_text = action->text();
    action_text.remove(QLatin1Char('&'));
    if (action_text == text) {
      return action;
    }
  }
  return nullptr;
}

// Splits the placeholder inside `dock` and returns the newly created sibling.
PJ::DockWidget* splitFrom(PJ::PlotDocker& docker, PJ::DockWidget* dock, int expected_count) {
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  EXPECT_NE(placeholder, nullptr);
  if (placeholder == nullptr) {
    return nullptr;
  }
  auto* split = findActionByText(placeholder, QStringLiteral("Split Horizontally"));
  EXPECT_NE(split, nullptr);
  if (split == nullptr) {
    return nullptr;
  }
  split->trigger();
  EXPECT_EQ(docker.plotCount(), expected_count);
  return docker.plotAt(expected_count - 1);
}

}  // namespace

// Focus behavior needs ADS's FocusController, which only exists when the
// FocusHighlighting config flag is set at CDockManager construction. The flag
// is process-global; set it per-test and restore it so test order can't leak
// focus behavior into tests that don't expect it.
class DockFocusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    saved_flags_ = ads::CDockManager::configFlags();
    ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
  }
  void TearDown() override {
    ads::CDockManager::setConfigFlags(saved_flags_);
  }

 private:
  ads::CDockManager::ConfigFlags saved_flags_;
};

TEST(DockWidgetPlaceholderTest, EmptyDockerStartsWithPlaceholderDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);

  ASSERT_EQ(docker.plotCount(), 1);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
}

TEST(DockWidgetPlaceholderTest, PlaceholderSplitActionsEmitRequests) {
  TestPlaceholderWidget placeholder;
  int horizontal_count = 0;
  int vertical_count = 0;
  QObject::connect(&placeholder, &PJ::VisualizationPlaceholderWidget::splitHorizontalRequested, &placeholder, [&]() {
    ++horizontal_count;
  });
  QObject::connect(&placeholder, &PJ::VisualizationPlaceholderWidget::splitVerticalRequested, &placeholder, [&]() {
    ++vertical_count;
  });

  auto* horizontal_action = findActionByText(&placeholder, QStringLiteral("Split Horizontally"));
  auto* vertical_action = findActionByText(&placeholder, QStringLiteral("Split Vertically"));
  ASSERT_NE(horizontal_action, nullptr);
  ASSERT_NE(vertical_action, nullptr);

  horizontal_action->trigger();
  vertical_action->trigger();

  EXPECT_EQ(horizontal_count, 1);
  EXPECT_EQ(vertical_count, 1);
}

TEST(DockWidgetPlaceholderTest, PlaceholderSplitActionCreatesSiblingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);

  ASSERT_EQ(docker.plotCount(), 1);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  auto* placeholder = dock->findChild<PJ::VisualizationPlaceholderWidget*>();
  ASSERT_NE(placeholder, nullptr);
  auto* horizontal_action = findActionByText(placeholder, QStringLiteral("Split Horizontally"));
  ASSERT_NE(horizontal_action, nullptr);

  horizontal_action->trigger();

  EXPECT_EQ(docker.plotCount(), 2);
}

TEST_F(DockFocusTest, DropFocusesReceivingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.show();  // focusedDockWidgetChanged only fires for visible docks.

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Drop a scalar onto the second dock — it should immediately gain focus so
  // its settings show, rather than waiting for a manual click.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock1, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
}

TEST_F(DockFocusTest, ObjectDropFocusesReceivingDock) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();
  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.setObjectWidgetFactory(
      [](const QString& /*kind*/, const PJ::ObjectDropSeed* /*seed*/, QWidget* parent) -> PJ::IDataWidget* {
        return new FakeObjectWidget(parent);
      });
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Dropping an object topic (factory-created widget) focuses its dock too.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          dock1, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key})));

  ASSERT_NE(dock1->objectWidget(), nullptr);
  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
}

TEST_F(DockFocusTest, ClosingFocusedDockRefocusesPreviouslyFocused) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  auto* dock1 = splitFrom(docker, dock0, 2);
  ASSERT_NE(dock1, nullptr);
  auto* dock2 = splitFrom(docker, dock1, 3);
  ASSERT_NE(dock2, nullptr);

  // Establish focus history: … → dock1 → dock2, so the previous is dock1.
  docker.setDockWidgetFocused(dock1);
  docker.setDockWidgetFocused(dock2);
  ASSERT_EQ(docker.focusedDock(), dock2);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  dock2->closeDockWidget();

  // Focus returns to the previously focused dock (dock1), not merely the first
  // surviving sibling (dock0) — and never stays on the deleted dock.
  EXPECT_EQ(focused, dock1);
  EXPECT_EQ(docker.focusedDock(), dock1);
  EXPECT_EQ(docker.plotCount(), 2);
}

TEST_F(DockFocusTest, ClosingLastRealDockFocusesPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.show();

  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  docker.setDockWidgetFocused(dock0);

  PJ::DockWidget* focused = dock0;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  dock0->closeDockWidget();

  // A fresh placeholder is created and focused; it is not the deleted dock,
  // so the panel falls back to its empty page instead of stale settings.
  ASSERT_EQ(docker.plotCount(), 1);
  EXPECT_NE(focused, dock0);
  EXPECT_EQ(focused, docker.plotAt(0));
}

TEST_F(DockFocusTest, DropOntoAlreadyFocusedPlaceholderRefreshes) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  ASSERT_NE(addScalarTopic(session, *dataset, "/imu/accel"), 0U);
  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.show();

  // Close the only widget: a fresh placeholder is created and *takes focus*.
  auto* dock0 = docker.plotAt(0);
  ASSERT_NE(dock0, nullptr);
  docker.setDockWidgetFocused(dock0);
  dock0->closeDockWidget();
  auto* placeholder = docker.plotAt(0);
  ASSERT_NE(placeholder, nullptr);
  ASSERT_EQ(docker.focusedDock(), placeholder);

  PJ::DockWidget* focused = nullptr;
  QObject::connect(&docker, &PJ::PlotDocker::dockFocused, [&](PJ::DockWidget* d) { focused = d; });

  // Dropping onto the already-focused placeholder populates it; ADS won't emit a
  // focus change (focus didn't move), but the panel must still refresh.
  ASSERT_TRUE(
      QMetaObject::invokeMethod(
          placeholder, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name})));

  ASSERT_NE(placeholder->plotWidget(), nullptr);
  EXPECT_EQ(focused, placeholder);
}

TEST(DockWidgetPlaceholderTest, ScalarDropConvertsPlaceholderToPlot) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  const bool invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{curves[0].name}));
  ASSERT_TRUE(invoked);

  ASSERT_NE(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_EQ(dock->plotWidget()->curveList().size(), 1U);
}

TEST(DockWidgetPlaceholderTest, CurveListChangedSeesDisplayTitleAfterCatalogKeyAdd) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const PJ::TopicId topic_id = addScalarTopic(session, *dataset, "/imu/accel");
  ASSERT_NE(topic_id, 0U);

  const auto curves = catalog.curves();
  ASSERT_EQ(curves.size(), 1U);
  ASSERT_NE(curves[0].name, QStringLiteral("imu/accel/value"));

  PJ::PlotWidget plot(&session, &catalog);
  QStringList observed_titles;
  QObject::connect(&plot, &PJ::PlotWidget::curveListChanged, &plot, [&]() {
    for (const auto& info : plot.curveList()) {
      if (info.curve != nullptr) {
        observed_titles << info.curve->title().text();
      }
    }
  });

  const auto* info = plot.addCurve(curves[0].name);
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->source_name, curves[0].name);
  EXPECT_EQ(info->curve->title().text(), QStringLiteral("imu/accel/value"));
  EXPECT_EQ(observed_titles, QStringList{QStringLiteral("imu/accel/value")});
}

TEST(DockWidgetPlaceholderTest, ImageObjectDropConvertsPlaceholderToMedia2D) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);
  ASSERT_TRUE(PJ::isObjectTopic(items[0]));
  ASSERT_EQ(PJ::asObjectTopic(items[0])->object_type, PJ::sdk::BuiltinObjectType::kImage);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  bool factory_called = false;
  docker.setObjectWidgetFactory(
      [&](const QString& kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
        factory_called = true;
        // Drop path: empty kind, a seed carrying the dropped topic.
        EXPECT_TRUE(kind.isEmpty());
        EXPECT_NE(seed, nullptr);
        if (seed != nullptr) {
          EXPECT_EQ(seed->topic_id, *object_topic);
          EXPECT_EQ(seed->object_type, PJ::sdk::BuiltinObjectType::kImage);
          // Single dataset loaded -> title drops the redundant "drive.mcap/" prefix.
          EXPECT_EQ(seed->title, QStringLiteral("/camera/image_raw/compressed"));
        }
        return new FakeObjectWidget(parent);
      });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);

  const bool invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key}));
  ASSERT_TRUE(invoked);

  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_TRUE(factory_called);
  EXPECT_NE(dock->objectWidget(), nullptr);
}

TEST(DockWidgetPlaceholderTest, ClearObjectContentRestoresPlaceholder) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  auto object_topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/camera/image_raw/compressed",
          .metadata_json = R"({"builtin_object_type":"kImage"})",
      });
  ASSERT_TRUE(object_topic.has_value()) << object_topic.error();
  catalog.rebuildFromDatastore();

  const auto items = catalog.items();
  ASSERT_EQ(items.size(), 1U);

  PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);
  docker.setObjectWidgetFactory([](const QString&, const PJ::ObjectDropSeed*, QWidget* parent) -> PJ::IDataWidget* {
    return new FakeObjectWidget(parent);
  });
  auto* dock = docker.plotAt(0);
  ASSERT_NE(dock, nullptr);
  int undoable_count = 0;
  QObject::connect(dock, &PJ::DockWidget::undoableChange, dock, [&]() { ++undoable_count; });

  const bool drop_invoked = QMetaObject::invokeMethod(
      dock, "onCatalogItemsDropped", Qt::DirectConnection, Q_ARG(QStringList, QStringList{items[0].key}));
  ASSERT_TRUE(drop_invoked);
  ASSERT_NE(dock->objectWidget(), nullptr);
  EXPECT_EQ(undoable_count, 1);

  const bool clear_invoked = QMetaObject::invokeMethod(dock, "clearToPlaceholder", Qt::DirectConnection);
  ASSERT_TRUE(clear_invoked);

  EXPECT_EQ(dock->plotWidget(), nullptr);
  EXPECT_EQ(dock->objectWidget(), nullptr);
  EXPECT_NE(dock->findChild<PJ::VisualizationPlaceholderWidget*>(), nullptr);
  EXPECT_EQ(dock->name(), QStringLiteral("..."));
  EXPECT_EQ(undoable_count, 2);
}

TEST(DockWidgetPlaceholderTest, RestoreRoutesObjectWidgetTagsToFactoryByKind) {
  // Regression: a saved <scene2d> dock (and <scene3d>) must be recognized on
  // restore and routed to the object-widget factory keyed by its XML tag. Before
  // the generic-restore fix the layout parser collected only <plot> and
  // <scene3d>, so <scene2d> docks silently vanished on reload.
  for (const QString& kind : {QStringLiteral("scene2d"), QStringLiteral("scene3d")}) {
    PJ::SessionManager session;
    PJ::CatalogModel catalog(&session);
    PJ::PlotDocker docker(QStringLiteral("test"), &session, &catalog);

    QString seen_kind;
    bool seed_was_null = false;
    docker.setObjectWidgetFactory(
        [&](const QString& factory_kind, const PJ::ObjectDropSeed* seed, QWidget* parent) -> PJ::IDataWidget* {
          seen_kind = factory_kind;
          seed_was_null = (seed == nullptr);
          return new FakeObjectWidget(parent);
        });

    QDomDocument doc;
    const QString xml = QStringLiteral(
                            "<Tab id=\"t1\" containers=\"1\"><Container><DockArea id=\"a1\" name=\"View\">"
                            "<%1/></DockArea></Container></Tab>")
                            .arg(kind);
    ASSERT_TRUE(doc.setContent(xml));

    EXPECT_TRUE(docker.xmlLoadState(doc.documentElement()));
    EXPECT_EQ(seen_kind, kind);
    EXPECT_TRUE(seed_was_null);  // restore passes a null seed (the dock reloads from XML)
    auto* dock = docker.plotAt(0);
    ASSERT_NE(dock, nullptr);
    EXPECT_NE(dock->objectWidget(), nullptr);
    EXPECT_EQ(dock->plotWidget(), nullptr);
  }
}

TEST(DockWidgetPlaceholderTest, PlaceholderAcceptsCatalogDragMoveAndIconDrop) {
  TestPlaceholderWidget placeholder;
  int drop_count = 0;
  QStringList dropped_keys;
  QObject::connect(
      &placeholder, &PJ::VisualizationPlaceholderWidget::catalogItemsDropped, &placeholder,
      [&](const QStringList& keys) {
        ++drop_count;
        dropped_keys = keys;
      });

  QMimeData mime_data;
  mime_data.setData(
      PJ::CurveTreeView::catalogItemsMimeType(),
      PJ::CurveTreeView::encodeCatalogKeys(QStringList{QStringLiteral("dataset:/camera/image")}));
  QDragEnterEvent drag_enter(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  placeholder.sendDragEnter(&drag_enter);
  EXPECT_TRUE(drag_enter.isAccepted());

  QDragMoveEvent drag_move(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  placeholder.sendDragMove(&drag_move);
  EXPECT_TRUE(drag_move.isAccepted());

  QToolButton* icon_button = nullptr;
  for (auto* button : placeholder.findChildren<QToolButton*>()) {
    if (button->isEnabled()) {
      icon_button = button;
      break;
    }
  }
  ASSERT_NE(icon_button, nullptr);

  QDragMoveEvent icon_drag_move(QPoint(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  EXPECT_TRUE(placeholder.sendFilteredEvent(icon_button, &icon_drag_move));
  EXPECT_TRUE(icon_drag_move.isAccepted());

  QDropEvent icon_drop(QPointF(1, 1), Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier);
  EXPECT_TRUE(placeholder.sendFilteredEvent(icon_button, &icon_drop));
  EXPECT_TRUE(icon_drop.isAccepted());
  EXPECT_EQ(drop_count, 1);
  EXPECT_EQ(dropped_keys, QStringList{QStringLiteral("dataset:/camera/image")});
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
