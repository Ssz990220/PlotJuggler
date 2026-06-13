// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression tests for Scene3DDockWidget layout persistence (WP6):
//
//  - M.18: a FrameTransforms config topic (consumed as config, zero render
//    layers) round-trips through xmlSaveState/xmlLoadState as a <config_topic>
//    element and re-binds the dock's TF buffer on restore.
//  - M.55: layer/config identity is re-resolved by stable dataset source name,
//    not the load-order DatasetId, so a layout saved when the data file was
//    dataset id N restores in a session where the same file is dataset id M.
//  - M.19: xmlLoadState forces the lazily-created view so a saved explicit fixed
//    frame survives even when zero layers restore (view_ otherwise null).
//
// These exercise the dock through its public surface against a real
// SessionManager/DataEngine/ObjectStore + TransformService. The GL view is a
// QOpenGLWidget; its ctor defers all GL to initializeGL(), so constructing it
// headless (offscreen platform, never shown) is safe and touches no context.

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QString>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"

namespace {

// Creates an engine dataset with the given source name (re-resolution key) and
// registers a TF object topic under its id, with one owned payload so the
// descriptor is non-empty (topic_name set) and entryCount() > 0. Returns the
// engine-assigned DatasetId and the topic id.
struct DatasetTopic {
  PJ::DatasetId dataset_id = 0;
  PJ::ObjectTopicId topic_id;
};

DatasetTopic registerTfDataset(
    PJ::SessionManager& session, const std::string& source_name, const std::string& topic_name) {
  auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source_name, .time_domain_id = 0});
  EXPECT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = topic_name;
  const auto topic_or = session.objectStore().registerTopic(desc);
  EXPECT_TRUE(topic_or.has_value());
  EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x00}).has_value());
  return {dataset_id, *topic_or};
}

// M.18: a TF-only dock persists its config topic and re-binds its TF buffer on
// restore. M.55: restore resolves the topic by dataset source name even when the
// DatasetId differs from the saved one.
TEST(Scene3DDockPersistence, ConfigTopicRoundTripsAndResolvesBySource) {
  // --- Save session: the file is dataset id 1.
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  const DatasetTopic saved = registerTfDataset(save_session, "drive.dat", "/tf");

  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, QStringLiteral("tf")));
  ASSERT_TRUE(save_dock.layers().empty()) << "FrameTransforms must not create a render layer";

  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);
  ASSERT_FALSE(state.isNull());
  const QDomElement config_el = state.firstChildElement(QStringLiteral("config_topic"));
  ASSERT_FALSE(config_el.isNull()) << "config topic must be persisted (M.18)";
  EXPECT_EQ(config_el.attribute(QStringLiteral("topic_name")), QStringLiteral("/tf"));
  EXPECT_EQ(config_el.attribute(QStringLiteral("dataset_source")), QStringLiteral("drive.dat"));

  // --- Restore session: a decoy file loads FIRST, so the SAME file is now
  // dataset id 2 — the saved id 1 must NOT be trusted blindly.
  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  registerTfDataset(load_session, "decoy.dat", "/other");                             // dataset id 1
  const DatasetTopic reloaded = registerTfDataset(load_session, "drive.dat", "/tf");  // dataset id 2
  ASSERT_NE(reloaded.dataset_id, saved.dataset_id) << "test premise: load order changed the DatasetId";

  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));

  // The config topic resolved by source name and re-bound the TF buffer to the
  // NEW dataset id, not the stale saved one.
  EXPECT_TRUE(load_dock.hasTransformBufferForTest());
  EXPECT_EQ(load_dock.boundDatasetIdForTest(), reloaded.dataset_id);
}

// M.55: when the saved dataset is absent entirely, restore drops the config
// topic gracefully (no binding) instead of resolving against a wrong dataset.
TEST(Scene3DDockPersistence, ConfigTopicUnresolvedWhenDatasetMissing) {
  PJ::SessionManager save_session;
  pj::scene3d::TransformService save_tf(save_session);
  const DatasetTopic saved = registerTfDataset(save_session, "drive.dat", "/tf");

  PJ::Scene3DDockWidget save_dock;
  save_dock.setSessionManager(&save_session);
  save_dock.setTransformService(&save_tf);
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, QStringLiteral("tf")));
  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);

  // Restore into a session that loaded a DIFFERENT file: nothing to resolve.
  PJ::SessionManager load_session;
  pj::scene3d::TransformService load_tf(load_session);
  registerTfDataset(load_session, "unrelated.dat", "/other");

  PJ::Scene3DDockWidget load_dock;
  load_dock.setSessionManager(&load_session);
  load_dock.setTransformService(&load_tf);
  ASSERT_TRUE(load_dock.xmlLoadState(state));
  EXPECT_FALSE(load_dock.hasTransformBufferForTest()) << "no TF binding when the saved dataset is absent";
}

// M.19: a saved explicit fixed frame is applied on restore even when zero layers
// restore, because xmlLoadState forces the lazily-created view first.
TEST(Scene3DDockPersistence, ExplicitFixedFrameSurvivesZeroLayerRestore) {
  // Hand-build a scene3d state element with an explicit fixed frame and no
  // layers / config topics (the "saved before the data file" case).
  QDomDocument doc;
  QDomElement state = doc.createElement(QStringLiteral("scene3d"));
  state.setAttribute(QStringLiteral("version"), QStringLiteral("1"));
  state.setAttribute(QStringLiteral("fixed_frame_mode"), QStringLiteral("explicit"));
  state.setAttribute(QStringLiteral("fixed_frame"), QStringLiteral("map"));
  doc.appendChild(state);

  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));

  // The view was forced into existence and the explicit frame applied despite no
  // layers — previously view_ stayed null and the frame was silently dropped.
  EXPECT_FALSE(dock.isAutoRootMode()) << "explicit fixed-frame mode must survive restore (M.19)";
  ASSERT_NE(dock.sceneView(), nullptr) << "restore must force the lazily-created view";
  EXPECT_EQ(dock.currentFixedFrame(), QStringLiteral("map"));
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
