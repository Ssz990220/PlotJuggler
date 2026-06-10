// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QString>
#include <QWidget>
#include <QtGlobal>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_datastore/engine.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"  // PJ::Timepoint, PJ::Range, fromRaw/toRaw
#include "pj_scene_common/layer_factory.h"
#include "pj_scene_common/scene_dock_widget.h"
#include "pj_scene_common/scene_layer.h"

namespace {

struct FakeLayerConfig {
  std::pair<int64_t, int64_t> range{0, 0};
};

std::unordered_map<uint32_t, FakeLayerConfig> g_fake_layer_configs;
std::vector<uint32_t> g_detached_topics;
// Ordered lifecycle log ("sync:<n>" / "detach:<id>" / "destroy:<id>") for
// order-of-operations assertions (e.g. view re-pointed before layers die).
std::vector<std::string> g_layer_events;

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

  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override {
    return {PJ::fromRaw(range_.first), PJ::fromRaw(range_.second)};
  }

  bool attach(const PJ::SceneLayerContext& ctx) override {
    attached_ = true;
    attached_session_ = ctx.session;
    return true;
  }

  ~FakeLayer() override {
    g_layer_events.push_back("destroy:" + std::to_string(info_.topic_id.id));
  }

  void detach() override {
    detached_ = true;
    g_detached_topics.push_back(info_.topic_id.id);
    g_layer_events.push_back("detach:" + std::to_string(info_.topic_id.id));
  }

  void setTrackerTime(PJ::Timepoint time) override {
    tracker_times_ns_.push_back(PJ::toRaw(time));
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

  /// Makes the dock consume topics of this object type as scene-wide config
  /// (no layer created), exercising the ConsumedAsConfig path.
  void setConfigConsumeType(PJ::sdk::BuiltinObjectType type) {
    config_consume_type_ = type;
  }

 protected:
  QWidget* createSceneView() override {
    return new QWidget();
  }

  bool handleSceneConfigTopic(
      PJ::ObjectTopicId /*topic_id*/, PJ::sdk::BuiltinObjectType object_type, const QString& /*title*/) override {
    return object_type == config_consume_type_;
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
    g_layer_events.push_back("sync:" + std::to_string(ordered_layers.size()));
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
  PJ::sdk::BuiltinObjectType config_consume_type_ = PJ::sdk::BuiltinObjectType::kNone;
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

  dock.setLayerVisible(topic(2), false);
  dock.onTrackerTime(50.0 / 1000000000.0);

  EXPECT_EQ(layer_a->trackerTimesNs(), (std::vector<int64_t>{100}));
  EXPECT_TRUE(layer_b->trackerTimesNs().empty());

  layer_a->clearTrackerTimes();
  dock.setLayerVisible(topic(1), false);
  dock.setLayerVisible(topic(2), true);
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
  source.setLayerVisible(*topic_a, false);
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

TEST(SceneDockWidgetTest, LayerAddedIsEmittedAfterViewIsReconciled) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  std::vector<int64_t> synced_at_emit;
  bool fired = false;
  // Mutate -> reconcile view -> notify: the view must already include the new
  // layer when observers (which may trigger a paint) react to layerAdded.
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) {
    synced_at_emit = dock.lastSyncedIds();
    fired = true;
  });

  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));

  EXPECT_TRUE(fired);
  EXPECT_EQ(synced_at_emit, (std::vector<int64_t>{1}));
}

TEST(SceneDockWidgetTest, LayerRemovedIsEmittedAfterViewIsReconciled) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_b")));

  std::vector<int64_t> synced_at_emit;
  bool fired = false;
  // The view must already exclude the removed layer when observers react, so a
  // paint-triggering slot never renders a composite bound to the detached layer.
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, &dock, [&](PJ::ObjectTopicId) {
    synced_at_emit = dock.lastSyncedIds();
    fired = true;
  });

  dock.removeTopic(topic(1));

  EXPECT_TRUE(fired);
  EXPECT_EQ(synced_at_emit, (std::vector<int64_t>{2}));
}

// Catalog-removal cleanup (IObjectViewer): revalidateObjects() drops layers
// whose ObjectStore topic was evicted and reports whether any live layer
// remains, so the shell can reset an emptied dock to its placeholder.
TEST(SceneDockWidgetTest, RevalidateObjectsPrunesEvictedTopicsAndReportsEmpty) {
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

  FakeSceneDock dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(*topic_a, PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("a")));
  ASSERT_TRUE(dock.addTopic(*topic_b, PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("b")));
  std::vector<uint32_t> removed_topics;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerRemoved, [&removed_topics](PJ::ObjectTopicId topic_id) {
    removed_topics.push_back(topic_id.id);
  });

  // Nothing evicted: everything stays, dock reports non-empty.
  EXPECT_TRUE(dock.revalidateObjects());
  EXPECT_EQ(dock.layers().size(), 2U);

  // Evict one topic: its layer is pruned (with the usual removal notification),
  // the other survives, dock still reports non-empty.
  session.objectStore().removeTopic(*topic_a);
  EXPECT_TRUE(dock.revalidateObjects());
  ASSERT_EQ(dock.layers().size(), 1U);
  EXPECT_EQ(dock.layers().front().topic_id.id, topic_b->id);
  EXPECT_EQ(removed_topics, (std::vector<uint32_t>{topic_a->id}));

  // Evict the last topic: dock empties and reports false (shell resets it).
  session.objectStore().removeTopic(*topic_b);
  EXPECT_FALSE(dock.revalidateObjects());
  EXPECT_TRUE(dock.layers().empty());
}

// A latched / one-shot layer (first == last, e.g. a map pinned to the recording
// start) is valid from its stamp ONWARD: it lowers the clamp's minimum but must
// NOT cap the maximum, or its lone early stamp would drag the live playhead
// backwards (the old "TF doesn't render unless another topic is present" bug).
TEST(SceneDockWidgetTest, LatchedLayerDoesNotDragTrackerTimeBack) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{1'000, 1'000}};  // latched
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("map")));
  auto* latched = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(latched, nullptr);

  // Live playhead after the latched stamp: must pass through unclamped.
  latched->clearTrackerTimes();
  dock.onTrackerTime(5'000.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{5'000}));

  // Before the latched stamp: raised to it (the slider minimum snaps onto it).
  latched->clearTrackerTimes();
  dock.onTrackerTime(500.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{1'000}));

  // A spanning layer bounds the top; the latched layer still does not extend it.
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{2'000, 3'000}};
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud")));
  latched->clearTrackerTimes();
  dock.onTrackerTime(10'000.0 / 1'000'000'000.0);
  EXPECT_EQ(latched->trackerTimesNs(), (std::vector<int64_t>{3'000}));
}

// A static layer (inverted/empty timeRange, e.g. a future URDF RobotModelLayer)
// must NOT contribute to the dock's clamp union: clampToLayerRange skips
// `range.max < range.min`, so a co-resident spanning layer alone bounds the
// playhead, and the static layer still receives every tracker tick.
TEST(SceneDockWidgetTest, StaticLayerWithInvertedRangeIsSkippedByClamp) {
  g_fake_layer_configs.clear();
  // Inverted range == "static / no data": max() as min field, min() as max field.
  g_fake_layer_configs[1] = FakeLayerConfig{
      std::pair<int64_t, int64_t>{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::lowest()}};
  g_fake_layer_configs[2] = FakeLayerConfig{std::pair<int64_t, int64_t>{2'000, 3'000}};  // spanning
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("static")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud")));
  auto* static_layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(static_layer, nullptr);
  static_layer->clearTrackerTimes();

  // Below the spanning layer's range -> raised to its lower bound (the static
  // layer's inverted range neither lowers nor caps).
  dock.onTrackerTime(1'000.0 / 1'000'000'000.0);
  EXPECT_EQ(static_layer->trackerTimesNs(), (std::vector<int64_t>{2'000}));

  // Inside the spanning layer's range -> passes through; static layer still ticks.
  static_layer->clearTrackerTimes();
  dock.onTrackerTime(2'500.0 / 1'000'000'000.0);
  EXPECT_EQ(static_layer->trackerTimesNs(), (std::vector<int64_t>{2'500}));
}

// Clearing all layers (the xmlLoadState restore path) must re-point the
// concrete view off the old layers (a sync with an empty order) BEFORE any of
// them is destroyed: a view holding raw layer pointers (e.g. the 3D
// SceneViewWidget's layer list) otherwise dereferences freed layers when it
// reconciles, and the layers' GL resources die without a release hook.
TEST(SceneDockWidgetTest, ClearLayersRepointsViewBeforeDestroyingLayers) {
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("a")));
  ASSERT_TRUE(dock.addTopic(topic(2), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("b")));
  g_layer_events.clear();

  QDomDocument doc;
  const QDomElement empty_scene = doc.createElement(QStringLiteral("scene"));
  ASSERT_TRUE(dock.xmlLoadState(empty_scene));

  const auto begin = g_layer_events.begin();
  const auto end = g_layer_events.end();
  const auto first_empty_sync = std::find(begin, end, std::string("sync:0"));
  const auto first_destroy =
      std::find_if(begin, end, [](const std::string& event) { return event.rfind("destroy:", 0) == 0; });
  ASSERT_NE(first_empty_sync, end) << "clearLayers() never re-pointed the view (no empty sync)";
  ASSERT_NE(first_destroy, end);
  EXPECT_LT(first_empty_sync - begin, first_destroy - begin)
      << "view was re-pointed only AFTER layers were destroyed (use-after-free window)";
  EXPECT_TRUE(dock.layers().empty());
}

TEST(SceneDockWidgetTest, TrackerTimeIgnoresNonFiniteValues) {
  g_fake_layer_configs.clear();
  g_fake_layer_configs[1] = FakeLayerConfig{std::pair<int64_t, int64_t>{100, 200}};
  FakeSceneDock dock;
  ASSERT_TRUE(dock.addTopic(topic(1), PJ::sdk::BuiltinObjectType::kPointCloud, QStringLiteral("cloud_a")));
  auto* layer = dynamic_cast<FakeLayer*>(dock.layerFor(topic(1)));
  ASSERT_NE(layer, nullptr);
  layer->clearTrackerTimes();

  // NaN/inf carry no position; the dock must drop them rather than cast a
  // non-finite double to int64 (undefined behavior) and forward garbage.
  dock.onTrackerTime(std::numeric_limits<double>::quiet_NaN());
  dock.onTrackerTime(std::numeric_limits<double>::infinity());

  EXPECT_TRUE(layer->trackerTimesNs().empty());
}

TEST(SceneDockWidgetTest, ConfigTopicIsConsumedWithoutCreatingLayer) {
  g_fake_layer_configs.clear();
  FakeSceneDock dock;
  dock.setConfigConsumeType(PJ::sdk::BuiltinObjectType::kImage);
  int added_count = 0;
  QObject::connect(&dock, &PJ::SceneDockWidget::layerAdded, &dock, [&](PJ::ObjectTopicId) { ++added_count; });

  // A scene-wide config topic is accepted (addTopic == true) but spawns no layer
  // and no layerAdded signal: the two meanings the typed AddOutcome separates.
  EXPECT_TRUE(dock.addTopic(topic(5), PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("config")));

  EXPECT_EQ(added_count, 0);
  EXPECT_EQ(dock.layerFor(topic(5)), nullptr);
  EXPECT_TRUE(dock.layers().empty());
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
