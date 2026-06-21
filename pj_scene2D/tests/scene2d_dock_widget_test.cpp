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

#include "pj_base/builtin/image.hpp"
#include "pj_base/builtin/image_codec.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene2d_widgets/Scene2DDockWidget.h"
#include "pj_scene2d_widgets/layers/depth_image_layer.h"
#include "pj_scene2d_widgets/layers/image_layer.h"

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

// Serialize a canonical sdk::Image — the bytes a no-parser image topic stores.
std::vector<uint8_t> makeImageBytes(
    uint32_t width, uint32_t height, const std::string& encoding, std::vector<uint8_t> pixels) {
  PJ::sdk::Image img;
  img.timestamp_ns = 100;
  img.width = width;
  img.height = height;
  img.encoding = encoding;
  img.data = PJ::Span<const uint8_t>(pixels.data(), pixels.size());
  return PJ::serializeImage(img);
}

// The layer family ("Depth" / "Image") the dock created for a topic.
QString familyFor(const std::vector<PJ::SceneLayerInfo>& layers, PJ::ObjectTopicId topic) {
  for (const auto& layer : layers) {
    if (layer.topic_id.id == topic.id) {
      return layer.family_name;
    }
  }
  return {};
}

}  // namespace

TEST(DepthImageLayer, XmlRoundTripPreservesColormapInvertAndRange) {
  PJ::DepthImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("depth"));

  QDomDocument doc;
  QDomElement in = doc.createElement(QStringLiteral("scene2d_layer"));
  in.setAttribute(QStringLiteral("colormap"), QStringLiteral("plasma"));
  in.setAttribute(QStringLiteral("invert"), QStringLiteral("true"));
  in.setAttribute(QStringLiteral("near_m"), QStringLiteral("1.5"));
  in.setAttribute(QStringLiteral("far_m"), QStringLiteral("7"));
  ASSERT_TRUE(layer.xmlLoadState(in));

  // Saving after loading must reproduce every depth-display attribute verbatim.
  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(QStringLiteral("colormap")), QStringLiteral("plasma"));
  EXPECT_EQ(out.attribute(QStringLiteral("invert")), QStringLiteral("true"));
  EXPECT_FLOAT_EQ(out.attribute(QStringLiteral("near_m")).toFloat(), 1.5f);
  EXPECT_FLOAT_EQ(out.attribute(QStringLiteral("far_m")).toFloat(), 7.0f);
}

TEST(ImageLayer, XmlRoundTripPreservesRectifyEnabled) {
  // The rectify override is the user-facing half of the feature: turn it off, save the
  // layout, reopen -> the override must survive. Pin the save/load symmetry so a
  // future mis-typed key or flipped comparison can't silently drop it.
  PJ::ImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image"));

  QDomDocument doc;
  QDomElement in = doc.createElement(QStringLiteral("scene2d_layer"));
  in.setAttribute(QStringLiteral("rectify_enabled"), QStringLiteral("false"));
  ASSERT_TRUE(layer.xmlLoadState(in));

  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(QStringLiteral("rectify_enabled")), QStringLiteral("false"));
}

TEST(ImageLayer, XmlLoadDefaultsRectifyOnWhenAttributeAbsent) {
  // A layout saved before this toggle existed has no rectify_enabled attribute; it
  // must default to ON, preserving the historical always-on rectification behaviour.
  PJ::ImageLayer layer(PJ::ObjectTopicId{1}, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("image"));

  QDomDocument doc;
  QDomElement in = doc.createElement(QStringLiteral("scene2d_layer"));  // no rectify_enabled attribute
  ASSERT_TRUE(layer.xmlLoadState(in));

  const QDomElement out = layer.xmlSaveState(doc);
  EXPECT_EQ(out.attribute(QStringLiteral("rectify_enabled")), QStringLiteral("true"));
}

TEST(Scene2DDockWidget, RoutesDepthEncodedImageToDepthLayer) {
  PJ::SessionManager session;
  const auto depth = registerTopic(session, 1, "/camera/depth/image");
  const auto color = registerTopic(session, 1, "/camera/color/image");
  // One canonical sdk::Image sample each (no parser -> the dock resolves the first
  // sample via the canonical codec to read its encoding).
  ASSERT_TRUE(session.objectStore()
                  .pushOwned(depth, 100, makeImageBytes(2, 2, "16UC1", std::vector<uint8_t>(2 * 2 * 2, 0x10)))
                  .has_value());
  ASSERT_TRUE(session.objectStore()
                  .pushOwned(color, 100, makeImageBytes(2, 2, "rgb8", std::vector<uint8_t>(2 * 2 * 3, 0x20)))
                  .has_value());

  PJ::Scene2DDockWidget dock;
  dock.setSessionManager(&session);
  ASSERT_TRUE(dock.addTopic(depth, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("depth")));
  ASSERT_TRUE(dock.addTopic(color, PJ::sdk::BuiltinObjectType::kImage, QStringLiteral("color")));

  // A depth-encoded kImage routes to the colormap DepthImageLayer; a color kImage
  // (same type) stays on the plain ImageLayer.
  EXPECT_EQ(familyFor(dock.layers(), depth), QStringLiteral("Depth"));
  EXPECT_EQ(familyFor(dock.layers(), color), QStringLiteral("Image"));
}

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
