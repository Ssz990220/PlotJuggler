// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QDomDocument>
#include <QDomElement>
#include <QtGlobal>
#include <cstdint>
#include <string>
#include <vector>

#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"

namespace {

PJ::ObjectTopicId registerTopic(PJ::SessionManager& session, uint32_t dataset_id, const std::string& topic_name) {
  // The dock now re-resolves a saved layer's dataset by source name on restore
  // (M.55), so the dataset must exist in the engine — mirror production, where
  // the ObjectStore and DataEngine are kept in lockstep. Registering the same id
  // twice is a harmless no-op (first writer wins).
  (void)session.dataEngine().createDataset(
      PJ::DatasetDescriptor{.source_name = "dataset_" + std::to_string(dataset_id), .time_domain_id = 0}, dataset_id);
  auto topic = session.objectStore().registerTopic(PJ::ObjectTopicDescriptor{dataset_id, topic_name, {}});
  EXPECT_TRUE(topic.has_value());
  return topic.has_value() ? *topic : PJ::ObjectTopicId{};
}

std::vector<uint32_t> ids(const std::vector<PJ::ObjectTopicId>& topics) {
  std::vector<uint32_t> out;
  out.reserve(topics.size());
  for (const auto topic : topics) {
    out.push_back(topic.id);
  }
  return out;
}

std::vector<uint32_t> layerIds(const std::vector<PJ::SceneLayerInfo>& layers) {
  std::vector<uint32_t> out;
  out.reserve(layers.size());
  for (const auto& layer : layers) {
    out.push_back(layer.topic_id.id);
  }
  return out;
}

}  // namespace

TEST(Scene2DDockWidget, CompositeTracksVisibilityAndOrder) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 1, "/camera/image");
  const auto depth = registerTopic(session, 1, "/camera/depth");

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image")));
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kDepthImage, QStringLiteral("depth")));

  EXPECT_EQ(dock.compositeLayerCountForTesting(), 2U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{image.id, depth.id}));

  dock.setLayerVisible(depth, false);
  EXPECT_EQ(dock.compositeLayerCountForTesting(), 1U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{image.id}));

  dock.setLayerVisible(depth, true);
  dock.reorderLayers({depth, image});
  EXPECT_EQ(dock.compositeLayerCountForTesting(), 2U);
  EXPECT_EQ(ids(dock.compositeTopicOrderForTesting()), (std::vector<uint32_t>{depth.id, image.id}));
  EXPECT_EQ(layerIds(dock.layers()), (std::vector<uint32_t>{depth.id, image.id}));
}

TEST(Scene2DDockWidget, XmlRoundTripRestoresLayerOrderAndVisibility) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 3, "/camera/image");
  const auto annotations = registerTopic(session, 3, "/camera/annotations");

  PJ::Scene2DDockWidget original;
  original.setSessionManager(&session);
  ASSERT_TRUE(original.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image")));
  ASSERT_TRUE(
      original.addTopic(annotations, PJ::sdk::BuiltinObjectType::kImageAnnotations, QStringLiteral("annotations")));

  original.reorderLayers({annotations, image});
  original.setLayerVisible(image, false);

  QDomDocument doc(QStringLiteral("scene2d"));
  const QDomElement saved = original.xmlSaveState(doc);
  doc.appendChild(saved);

  PJ::Scene2DDockWidget restored;
  restored.setSessionManager(&session);
  ASSERT_TRUE(restored.xmlLoadState(saved));

  EXPECT_EQ(layerIds(restored.layers()), (std::vector<uint32_t>{annotations.id, image.id}));
  EXPECT_EQ(restored.compositeLayerCountForTesting(), 1U);
  EXPECT_EQ(ids(restored.compositeTopicOrderForTesting()), (std::vector<uint32_t>{annotations.id}));

  const auto restored_layers = restored.layers();
  ASSERT_EQ(restored_layers.size(), 2U);
  EXPECT_TRUE(restored_layers[0].visible);
  EXPECT_FALSE(restored_layers[1].visible);
}

TEST(Scene2DDockWidget, EmptyPlaceholderActiveUntilFirstVisibleLayer) {
  PJ::SessionManager session;
  const auto image = registerTopic(session, 5, "/camera/image");

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  // A fresh, empty dock shows the placeholder rather than a blank GPU surface.
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());

  // The first visible layer fronts the viewer instead.
  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image")));
  EXPECT_FALSE(dock.emptyPlaceholderActiveForTesting());

  // Hiding the only layer empties the composite, so the placeholder returns.
  dock.setLayerVisible(image, false);
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());

  // Showing it again brings the viewer back.
  dock.setLayerVisible(image, true);
  EXPECT_FALSE(dock.emptyPlaceholderActiveForTesting());

  // Removing the last layer also returns to the placeholder.
  dock.removeTopic(image);
  EXPECT_TRUE(dock.emptyPlaceholderActiveForTesting());
}

TEST(Scene2DDockWidget, RevalidateKeepsNeverPopulatedDockButResetsEvictedOne) {
  PJ::SessionManager session;

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);

  // A click-created, never-populated dock must survive catalog churn (e.g. a
  // later dataset load), so the shell does NOT reset it to the placeholder.
  EXPECT_TRUE(dock.revalidateObjects());

  // Once it has held a topic and that topic is evicted, it reports empty so the
  // shell can reset it to the neutral placeholder — the original eviction behavior.
  const auto image = registerTopic(session, 9, "/camera/image");
  ASSERT_TRUE(dock.addTopic(image, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image")));
  EXPECT_TRUE(dock.revalidateObjects());  // live layer remains
  session.objectStore().removeTopic(image);
  EXPECT_FALSE(dock.revalidateObjects());  // had content, now evicted -> reset
}

TEST(Scene2DDockWidget, RevalidateWithoutSessionKeepsNeverPopulatedDock) {
  // No session set (e.g. mid teardown / session swap): a never-populated dock
  // must still be kept, not reported empty and wiped to the placeholder.
  PJ::Scene2DDockWidget dock;
  EXPECT_TRUE(dock.revalidateObjects());
}

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
