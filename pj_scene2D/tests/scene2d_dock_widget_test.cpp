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

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  ::testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
