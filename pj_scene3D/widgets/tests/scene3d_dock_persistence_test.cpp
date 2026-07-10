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
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "mock_parser_support.h"
#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/image.hpp"
#include "pj_base/dataset.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

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

using namespace pj::scene3d::test;

constexpr std::string_view kImageSchema = "mock/image";
const std::array<float, 4> kDepthPixels = {1.0f, 2.0f, 3.0f, 4.0f};

// A mock depth parser: ignores the payload and always yields a 32FC1 (depth) Image,
// so firstSampleIsDepthEncoded sees "depth" whenever a sample exists.
PJ::Expected<PJ::sdk::ObjectRecord> emitDepthImage(PJ::Timestamp ts, PJ::sdk::PayloadView /*p*/) {
  PJ::sdk::Image img;
  img.width = 2;
  img.height = 2;
  img.encoding = "32FC1";
  img.frame_id = "cam";
  img.timestamp_ns = ts;
  img.data = PJ::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kDepthPixels.data()), kDepthPixels.size() * 4U);
  return PJ::sdk::ObjectRecord{.ts = ts, .object = img};
}

// Like registerTfDataset, but for a depth-encoded kImage topic: registers a depth
// parser and, only when push_sample is true, pushes one sample. With
// push_sample=false the topic resolves (findTopic) but the ObjectStore holds no
// entry to peek — the "layout restored before the first frame arrives" case.
DatasetTopic registerDepthDataset(
    PJ::SessionManager& session, const std::string& source_name, const std::string& topic_name, bool push_sample) {
  auto dataset_or =
      session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = source_name, .time_domain_id = 0});
  EXPECT_TRUE(dataset_or.has_value());
  const PJ::DatasetId dataset_id = *dataset_or;

  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset_id;
  desc.topic_name = topic_name;
  desc.metadata_json = R"({"builtin_object_type":"kImage"})";
  const auto topic_or = session.objectStore().registerTopic(desc);
  EXPECT_TRUE(topic_or.has_value());
  if (push_sample) {
    EXPECT_TRUE(session.objectStore().pushOwned(*topic_or, 100, std::vector<uint8_t>{0x01}).has_value());
  }
  session.registerObjectTopicParser(*topic_or, makeBoundHandle(kImageSchema, []() noexcept -> void* {
    return new CountingObjectParser(kImageSchema, PJ::sdk::BuiltinObjectType::kImage, nullptr, &emitDepthImage);
  }));
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
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
  ASSERT_TRUE(save_dock.layers().empty()) << "FrameTransforms must not create a render layer";

  QDomDocument doc;
  const QDomElement state = save_dock.xmlSaveState(doc);
  ASSERT_FALSE(state.isNull());
  const QDomElement config_el = state.firstChildElement(u"config_topic"_s);
  ASSERT_FALSE(config_el.isNull()) << "config topic must be persisted (M.18)";
  EXPECT_EQ(config_el.attribute(u"topic_name"_s), u"/tf"_s);
  EXPECT_EQ(config_el.attribute(u"dataset_source"_s), u"drive.dat"_s);

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
  ASSERT_TRUE(save_dock.addTopic(saved.topic_id, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"tf"_s));
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
  QDomElement state = doc.createElement(u"scene3d"_s);
  state.setAttribute(u"version"_s, u"1"_s);
  state.setAttribute(u"fixed_frame_mode"_s, u"explicit"_s);
  state.setAttribute(u"fixed_frame"_s, u"map"_s);
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
  EXPECT_EQ(dock.currentFixedFrame(), u"map"_s);
}

// C1 regression: a saved DepthCloud (kImage) layer must restore even when its
// topic has no stored sample yet — the layout was applied before the first frame
// arrived (streaming, or a still-loading file). The interactive add path gates
// kImage on the first sample's encoding (firstSampleIsDepthEncoded), but on restore
// there may be no sample to peek; the layer was already validated as depth when the
// user created it, so restore must trust the saved type, not silently drop it (the
// drop was also invisible: unresolved_topics counts only dataset/topic-id
// resolution failures, which both succeeded here).
TEST(Scene3DDockPersistence, DepthCloudLayerRestoresBeforeFirstSample) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  // Topic resolves (dataset + topic registered, parser bound) but no sample yet.
  const DatasetTopic topic = registerDepthDataset(session, "cam.dat", "/cam/depth/image", /*push_sample=*/false);

  // Hand-built saved state: one kImage (DepthCloud) layer, resolvable by source.
  QDomDocument doc;
  QDomElement state = doc.createElement(u"scene3d"_s);
  state.setAttribute(u"version"_s, u"1"_s);
  QDomElement layer_el = doc.createElement(u"layer"_s);
  layer_el.setAttribute(u"object_type"_s, u"kImage"_s);
  layer_el.setAttribute(u"display_name"_s, u"depth"_s);
  layer_el.setAttribute(u"dataset_id"_s, QString::number(topic.dataset_id));
  layer_el.setAttribute(u"dataset_source"_s, u"cam.dat"_s);
  layer_el.setAttribute(u"topic_name"_s, u"/cam/depth/image"_s);
  state.appendChild(layer_el);
  doc.appendChild(state);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.xmlLoadState(state));

  EXPECT_EQ(dock.layers().size(), 1U) << "a saved DepthCloud layer must restore even before its first sample arrives "
                                         "(the encoding gate belongs to the interactive add path, not restore)";
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
