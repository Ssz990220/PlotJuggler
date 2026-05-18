#include <gtest/gtest.h>

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

}  // namespace

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
      [&](PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& title,
          QWidget* parent) -> PJ::IDataWidget* {
        factory_called = true;
        EXPECT_EQ(topic_id, *object_topic);
        EXPECT_EQ(object_type, PJ::sdk::BuiltinObjectType::kImage);
        EXPECT_EQ(title, QStringLiteral("drive.mcap//camera/image_raw/compressed"));
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
