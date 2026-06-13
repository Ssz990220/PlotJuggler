// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression tests for Scene3DDockWidget streaming / live-edge correctness:
//
//  - H.4: a TF-only dock (a FrameTransforms topic consumed as config, zero render
//    layers) must survive revalidateObjects() so MainWindow::syncWidgetsToCatalog
//    does not wipe it to a placeholder on every catalog change — and must report
//    "empty" only once its config topic is actually evicted from the store.
//  - M.17: the dock's TF buffer / dataset binding is not one-shot. After the bound
//    dataset's last tracked topic is removed, the binding resets so a topic from a
//    second dataset can rebind to the correct TF tree.
//
// These exercise the dock through its public IObjectViewer surface against a real
// SessionManager/ObjectStore + TransformService. The GL view is created lazily
// (a queued singleShot in the base ctor) and never realized here, so no OpenGL
// context is required: addTopic of a config topic and revalidateObjects touch the
// view only behind a `view_ != nullptr` guard.

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/transform_service.h"

namespace {

// Registers a topic in the store and pushes one owned payload so the descriptor
// is non-empty (topic_name set) and entryCount() > 0.
PJ::ObjectTopicId registerTfTopic(PJ::ObjectStore& store, PJ::DatasetId dataset_id, const std::string& name) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = name;
  const auto topic_id = store.registerTopic(desc);
  EXPECT_TRUE(topic_id.has_value());
  EXPECT_TRUE(store.pushOwned(*topic_id, 100, std::vector<uint8_t>{0x00}).has_value());
  return *topic_id;
}

TEST(Scene3DDockStreaming, TfOnlyDockSurvivesRevalidateUntilTopicEvicted) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const auto tf_topic = registerTfTopic(store, /*dataset_id=*/1, "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  // Drop a FrameTransforms topic: consumed as a config topic, creates NO layer.
  ASSERT_TRUE(dock.addTopic(tf_topic, PJ::sdk::BuiltinObjectType::kFrameTransforms, QStringLiteral("tf")));
  EXPECT_TRUE(dock.layers().empty()) << "a FrameTransforms topic must not create a render layer";

  // H.4: with zero layers but a live config topic, the dock is still alive — a
  // catalog change (here, just re-running the check) must not wipe it.
  EXPECT_TRUE(dock.revalidateObjects());
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 1u);

  // Evicting the config topic from the store leaves the dock genuinely empty.
  store.removeTopic(tf_topic);
  EXPECT_FALSE(dock.revalidateObjects());
  // The binding reset because no tracked topic remained for the dataset (M.17).
  EXPECT_FALSE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 0u);
}

TEST(Scene3DDockStreaming, RebindsToSecondDatasetAfterFirstRemoved) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();

  const auto tf_a = registerTfTopic(store, /*dataset_id=*/1, "/tf");

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  ASSERT_TRUE(dock.addTopic(tf_a, PJ::sdk::BuiltinObjectType::kFrameTransforms, QStringLiteral("tf")));
  ASSERT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 1u);

  // Remove dataset A's topic and revalidate: the binding must reset so the dock
  // is free to adopt a different dataset's TF tree.
  store.removeTopic(tf_a);
  EXPECT_FALSE(dock.revalidateObjects());
  ASSERT_FALSE(dock.hasTransformBufferForTest());

  // A topic from a second dataset now rebinds cleanly to dataset B's buffer
  // rather than silently resolving against A's dead tree.
  const auto tf_b = registerTfTopic(store, /*dataset_id=*/2, "/tf");
  ASSERT_TRUE(dock.addTopic(tf_b, PJ::sdk::BuiltinObjectType::kFrameTransforms, QStringLiteral("tf2")));
  EXPECT_TRUE(dock.hasTransformBufferForTest());
  EXPECT_EQ(dock.boundDatasetIdForTest(), 2u);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
