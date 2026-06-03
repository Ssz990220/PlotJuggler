#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"

namespace {

struct FakeLayerConfig {
  std::pair<int64_t, int64_t> range{0, 0};
};

std::unordered_map<uint32_t, FakeLayerConfig> g_fake_layer_configs;
std::vector<uint32_t> g_detached_topics;

PJ::ObjectTopicId topic(uint32_t id) {
  PJ::ObjectTopicId topic_id;
  topic_id.id = id;
  return topic_id;
}

std::vector<int64_t> infoIds(const std::vector<PJ::SceneLayerInfo>& infos) {
  std::vector<int64_t> ids;
  ids.reserve(infos.size());
  for (const auto& info : infos) {
    ids.push_back(static_cast<int64_t>(info.topic_id.id));
  }
  return ids;
}

class FakeLayer : public PJ::ISceneLayer {
 public:
  FakeLayer(PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name)
      : info_{topic_id, object_type, display_name, QStringLiteral("Fake"), true} {
    const auto it = g_fake_layer_configs.find(topic_id.id);
    if (it != g_fake_layer_configs.end()) {
      range_ = it->second.range;
    }
  }

  [[nodiscard]] PJ::SceneLayerInfo info() const override {
    return info_;
  }

  [[nodiscard]] std::pair<int64_t, int64_t> timeRangeNs() const override {
    return range_;
  }

  bool attach(const PJ::SceneLayerContext& ctx) override {
    attached_ = true;
    attached_session_ = ctx.session;
    return true;
  }

  void detach() override {
    detached_ = true;
    g_detached_topics.push_back(info_.topic_id.id);
  }

  void setTrackerTime(std::chrono::nanoseconds time) override {
    tracker_times_ns_.push_back(time.count());
  }

  void setVisible(bool visible) override {
    info_.visible = visible;
    emit visibilityChanged(visible);
  }

  QWidget* createConfigWidget(QWidget* parent) override {
    return new QWidget(parent);
  }

  QDomElement xmlSaveState(QDomDocument& doc) const override {
    QDomElement element = doc.createElement(QStringLiteral("fake"));
    element.setAttribute(QStringLiteral("payload"), payload_);
    return element;
  }

  bool xmlLoadState(const QDomElement& element) override {
    payload_ = element.attribute(QStringLiteral("payload"));
    return true;
  }

  void clearTrackerTimes() {
    tracker_times_ns_.clear();
  }

  [[nodiscard]] const std::vector<int64_t>& trackerTimesNs() const {
    return tracker_times_ns_;
  }

  [[nodiscard]] bool attached() const {
    return attached_;
  }

  [[nodiscard]] bool detached() const {
    return detached_;
  }

  [[nodiscard]] PJ::SessionManager* attachedSession() const {
    return attached_session_;
  }

  void setPayload(QString payload) {
    payload_ = std::move(payload);
  }

  [[nodiscard]] QString payload() const {
    return payload_;
  }

 private:
  PJ::SceneLayerInfo info_;
  std::pair<int64_t, int64_t> range_{0, 0};
  std::vector<int64_t> tracker_times_ns_;
  QString payload_;
  bool attached_ = false;
  bool detached_ = false;
  PJ::SessionManager* attached_session_ = nullptr;
};

class FakeSceneDock : public PJ::SceneDockWidget {
 public:
  FakeSceneDock() {
    layerFactory().registerType(
        PJ::sdk::BuiltinObjectType::kPointCloud,
        [](PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name) {
          return std::make_unique<FakeLayer>(topic_id, object_type, display_name);
        });
  }

  [[nodiscard]] const std::vector<int64_t>& lastSyncedIds() const {
    return last_synced_ids_;
  }

  [[nodiscard]] int refreshCount() const {
    return refresh_count_;
  }

 protected:
  QWidget* createSceneView() override {
    return new QWidget();
  }

  std::unique_ptr<PJ::SceneLayerContext> makeContext() override {
    auto context = std::make_unique<PJ::SceneLayerContext>();
    context->session = sessionManager();
    return context;
  }

  [[nodiscard]] bool acceptsObjectType(PJ::sdk::BuiltinObjectType object_type) const override {
    return object_type == PJ::sdk::BuiltinObjectType::kPointCloud;
  }

  void syncViewLayers(const std::vector<PJ::ISceneLayer*>& ordered_layers) override {
    last_synced_ids_.clear();
    last_synced_ids_.reserve(ordered_layers.size());
    for (const PJ::ISceneLayer* layer : ordered_layers) {
      last_synced_ids_.push_back(static_cast<int64_t>(layer->info().topic_id.id));
    }
  }

  void refreshView() override {
    ++refresh_count_;
  }

 private:
  std::vector<int64_t> last_synced_ids_;
  int refresh_count_ = 0;
};

}  // namespace

TEST(LayerFactoryTest, RegisterSupportsCreateAndUnsupportedReturnsNull) {
  PJ::LayerFactory factory;
  EXPECT_FALSE(factory.supports(PJ::sdk::BuiltinObjectType::kPointCloud));
  EXPECT_EQ(factory.create(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud")), nullptr);

  factory.registerType(
      PJ::sdk::BuiltinObjectType::kPointCloud,
      [](PJ::ObjectTopicId topic_id, PJ::sdk::BuiltinObjectType object_type, const QString& display_name) {
        return std::make_unique<FakeLayer>(topic_id, object_type, display_name);
      });

  EXPECT_TRUE(factory.supports(PJ::sdk::BuiltinObjectType::kPointCloud));
  auto layer = factory.create(topic(7), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud"));
  ASSERT_NE(layer, nullptr);
  EXPECT_EQ(layer->info().topic_id, topic(7));
  EXPECT_EQ(layer->info().display_name, QStringLiteral("cloud"));
  EXPECT_EQ(factory.create(topic(8), PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image")), nullptr);
}

TEST(SceneDockWidgetTest, AddTopicEmitsLayerAddedAndReportsDrawOrder) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  EXPECT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  EXPECT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_b")));

  EXPECT_EQ(added_count, 2);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{1, 2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{1, 2}));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);
  EXPECT_TRUE(layer->attached());
  EXPECT_EQ(layer->info().display_name, QStringLiteral("cloud_a"));
}

TEST(SceneDockWidgetTest, AddingTheSameTopicTwiceIsRejected) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  EXPECT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  // Re-adding the same topic must be rejected: no duplicate layer, no second
  // layerAdded signal. Drag-drop and XML restore both route through addTopic, so
  // this guards against the same topic appearing twice in one scene dock.
  EXPECT_FALSE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));

  EXPECT_EQ(added_count, 1);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{1}));
}

TEST(SceneDockWidgetTest, RemoveTopicDetachesAndEmitsLayerRemoved) {
  g_fake_layer_configs.clear();
  g_detached_topics.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_b")));

  int removed_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, &dock, [&](PJ::ObjectTopicId) { ++removed_count; });
  dock.removeTopic(topic(1));

  EXPECT_EQ(g_detached_topics, (std::vector<uint32_t>{1}));
  EXPECT_EQ(removed_count, 1);
  EXPECT_EQ(dock.layerFor(topic(1)), nullptr);
  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{2}));
}

TEST(SceneDockWidgetTest, ReorderLayersUpdatesPublicOrderAndSyncViewOrder) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_b")));
  ASSERT_TRUE(dock.addTopic(topic(3), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_c")));

  dock.reorderLayers({topic(3), topic(1), topic(2)});

  EXPECT_EQ(infoIds(dock.layers()), (std::vector<int64_t>{3, 1, 2}));
  EXPECT_EQ(dock.lastSyncedIds(), (std::vector<int64_t>{3, 1, 2}));
}

TEST(SceneDockWidgetTest, TrackerTimeClampsToLayerUnionAndForwardsToVisibleLayersOnly) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{100, 200}};
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{300, 400}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_b")));
  auto* layer_a = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  auto* layer_b = dynamic_cast<FakeLayer*>(dock.layerFor(topic(2)));
  ASSERT_NE(layer_a, nullptr);
  ASSERT_NE(layer_b, nullptr);
  layer_a->clearTrackerTimes();
  layer_b->clearTrackerTimes();

  dock.setTopicVisible(topic(2), false);
  dock.onTrackerTime(50.0 / 1000000000.0);

  EXPECT_EQ(layer_a->trackerTimesNs(), (std::vector<int64_t>{100}));
  EXPECT_TRUE(layer_b->trackerTimesNs().empty());

  layer_a->clearTrackerTimes();
  dock.setTopicVisible(topic(1), false);
  dock.setTopicVisible(topic(2), true);
  dock.onTrackerTime(500.0 / 1000000000.0);

  EXPECT_TRUE(layer_a->trackerTimesNs().empty());
  EXPECT_EQ(layer_b->trackerTimesNs(), (std::vector<int64_t>{400}));
}

TEST(SceneDockWidgetTest, XmlSaveLoadRoundTripsLayersOrderVisibilityAndPayload) {
  g_fake_layer_configs.clear();
  PJ::SessionManager session;
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();

  auto topic_a = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_a",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_a.has_value()) << topic_a.error();
  auto topic_b = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{
          .dataset_id = *dataset,
          .topic_name = "/cloud_b",
          .metadata_json = R"({"builtin_object_type":"kPointCloud"})",
      });
  ASSERT_TRUE(topic_b.has_value()) << topic_b.error();

  FakeSceneDock source;
  source.setSessionManager(&session);
  ASSERT_TRUE(source.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("Cloud A")));
  ASSERT_TRUE(source.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("Cloud B")));
  auto* source_a = dynamic_cast<FakeLayer*>(source.layerFor(*topic_a));
  auto* source_b = dynamic_cast<FakeLayer*>(source.layerFor(*topic_b));
  ASSERT_NE(source_a, nullptr);
  ASSERT_NE(source_b, nullptr);
  source_a->setPayload(QStringLiteral("alpha"));
  source_b->setPayload(QStringLiteral("beta"));
  source.setTopicVisible(*topic_a, false);
  source.reorderLayers({*topic_b, *topic_a});

  QDomDocument doc(QStringLiteral("scene_common"));
  const QDomElement root = source.xmlSaveState(doc);
  doc.appendChild(root);

  FakeSceneDock restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(doc.documentElement()));

  const auto infos = restored.layers();
  ASSERT_EQ(infos.size(), 2U);
  EXPECT_EQ(
      infoIds(infos), (std::vector<int64_t>{static_cast<int64_t>(topic_b->id), static_cast<int64_t>(topic_a->id)}));
  EXPECT_EQ(infos[0].display_name, QStringLiteral("Cloud B"));
  EXPECT_TRUE(infos[0].visible);
  EXPECT_EQ(infos[1].display_name, QStringLiteral("Cloud A"));
  EXPECT_FALSE(infos[1].visible);

  auto* restored_a = dynamic_cast<FakeLayer*>(restored.layerFor(*topic_a));
  auto* restored_b = dynamic_cast<FakeLayer*>(restored.layerFor(*topic_b));
  ASSERT_NE(restored_a, nullptr);
  ASSERT_NE(restored_b, nullptr);
  EXPECT_EQ(restored_a->payload(), QStringLiteral("alpha"));
  EXPECT_EQ(restored_b->payload(), QStringLiteral("beta"));
  EXPECT_EQ(restored.lastSyncedIds(), infoIds(infos));
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
