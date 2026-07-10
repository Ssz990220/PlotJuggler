// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/robot_model_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/time.hpp"
#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
using namespace Qt::StringLiterals;

namespace {

constexpr const char* kDecodedUrdf = R"(
<robot name="decoded">
  <link name="base_link">
    <visual>
      <geometry><box size="1 2 3"/></geometry>
    </visual>
  </link>
  <link name="tool0"/>
</robot>
)";

// A two-link robot whose child link is attached ONLY by a FIXED joint. Mirrors
// the DROID gripper case: the data publishes the parent frame but never the child,
// so the child renders only if the URDF's fixed joint is bridged into TF.
constexpr const char* kBridgeUrdf = R"(
<robot name="bridge">
  <link name="base_link"/>
  <joint name="mount" type="fixed">
    <parent link="base_link"/>
    <child link="gripper_base"/>
    <origin xyz="0 0 0.1"/>
  </joint>
  <link name="gripper_base"/>
</robot>
)";

// A mixed model: a link with a <visual> plus a collision-ONLY sublink attached by
// a fixed joint (mirrors panda_link*_sc self-collision capsules on the DROID arm).
constexpr const char* kMixedCollisionUrdf = R"(
<robot name="mixed">
  <link name="body">
    <visual><geometry><box size="1 1 1"/></geometry></visual>
  </link>
  <joint name="sc" type="fixed">
    <parent link="body"/><child link="body_sc"/><origin xyz="0 0 0"/>
  </joint>
  <link name="body_sc">
    <collision><geometry><sphere radius="0.5"/></geometry></collision>
  </link>
</robot>
)";

// An entirely-collision model (no <visual> anywhere): kAuto must promote it to the
// visuals group so it renders solid (review L.21).
constexpr const char* kAllCollisionUrdf = R"(
<robot name="allcol">
  <link name="solid">
    <collision><geometry><sphere radius="0.5"/></geometry></collision>
  </link>
</robot>
)";

constexpr const char* kUnresolvedMeshUrdf = R"(
<robot name="mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="package://missing_pkg/meshes/body.dae"/></geometry>
    </visual>
  </link>
</robot>
)";

// URDF whose single visual mesh resolves immediately (file:// fixture path), so
// attach() actually kicks an async MeshLoader load.
const std::string& resolvedMeshUrdf() {
  static const std::string text = std::string(R"(
<robot name="resolved_mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="file://)") +
                                  PJ_SCENE3D_FIXTURES_DIR + R"(/meshes/cube.stl"/></geometry>
    </visual>
  </link>
</robot>
)";
  return text;
}

// URDF whose single visual mesh is a resolved .dae (file:// fixture path). The
// COLLADA up-axis override only affects .dae/.collada, so this exercises the
// setIgnoreColladaUpAxis reload path.
const std::string& resolvedColladaMeshUrdf() {
  static const std::string text = std::string(R"(
<robot name="collada_mesh_robot">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="file://)") +
                                  PJ_SCENE3D_FIXTURES_DIR + R"(/meshes/cube.dae"/></geometry>
    </visual>
  </link>
</robot>
)";
  return text;
}

class UrdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  UrdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kDecodedUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class SdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  SdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "sdf",
              .text = "<sdf/>",
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class BridgeUrdfRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  BridgeUrdfRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kBridgeUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class MixedCollisionRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  MixedCollisionRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts, .topic = "/robot_description", .format = "urdf", .text = kMixedCollisionUrdf}},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class AllCollisionRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  AllCollisionRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts, .topic = "/robot_description", .format = "urdf", .text = kAllCollisionUrdf}},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class UnresolvedMeshRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  UnresolvedMeshRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kUnresolvedMeshUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

class ResolvedMeshRobotDescriptionParser final : public PJ::MessageParserPluginBase {
 public:
  ResolvedMeshRobotDescriptionParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = resolvedMeshUrdf(),
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// Rebind-test parse counters. vtableWithCreate() requires a non-capturing create
// function, so the counting parsers reach these via file scope rather than a
// lambda capture.
std::atomic<int> g_first_urdf_parser_calls{0};
std::atomic<int> g_second_urdf_parser_calls{0};
// Counts decodes of the resolved-collada URDF so the up-axis toggle test can
// prove setIgnoreColladaUpAxis re-decoded the source.
std::atomic<int> g_collada_urdf_parser_calls{0};

// Emits the resolved-collada URDF and counts each decode, for the up-axis toggle
// reload test.
class CountingColladaUrdfParser final : public PJ::MessageParserPluginBase {
 public:
  CountingColladaUrdfParser() {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object = [](PJ::Timestamp ts,
                              PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      g_collada_urdf_parser_calls.fetch_add(1);
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = resolvedColladaMeshUrdf(),
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// Counts parseObject invocations so a rebind test can prove which parser a
// re-decode reached. Emits a fixed minimal URDF so the layer still parses a
// valid model.
class CountingUrdfParser final : public PJ::MessageParserPluginBase {
 public:
  explicit CountingUrdfParser(std::atomic<int>* counter) {
    PJ::sdk::SchemaHandler handler;
    handler.object_type = PJ::sdk::BuiltinObjectType::kRobotDescription;
    handler.parse_object =
        [counter](PJ::Timestamp ts, PJ::sdk::PayloadView /*payload*/) -> PJ::Expected<PJ::sdk::ObjectRecord> {
      counter->fetch_add(1);
      return PJ::sdk::ObjectRecord{
          .ts = std::nullopt,
          .object = PJ::sdk::BuiltinObject{PJ::sdk::RobotDescription{
              .timestamp_ns = ts,
              .topic = "/robot_description",
              .format = "urdf",
              .text = kDecodedUrdf,
          }},
      };
    };
    registerSchemaHandler("robot_description", std::move(handler));
  }
};

// vtableWithCreate() holds one static vtable per CreateFn instantiation, so each
// counting parser needs its own distinct lambda type — a shared function-pointer
// type would latch the first counter for both handles.
template <typename CreateFn>
const PJ_message_parser_vtable_t* countingUrdfVtable(CreateFn create_fn) {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      create_fn,
      R"({"id":"robot-counting-urdf-test","name":"Robot Counting URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* urdfParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new UrdfRobotDescriptionParser(); },
      R"({"id":"robot-urdf-test","name":"Robot URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* sdfParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new SdfRobotDescriptionParser(); },
      R"({"id":"robot-sdf-test","name":"Robot SDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* bridgeParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new BridgeUrdfRobotDescriptionParser(); },
      R"({"id":"robot-bridge-test","name":"Robot Bridge URDF Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* mixedCollisionParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new MixedCollisionRobotDescriptionParser(); },
      R"({"id":"robot-mixed-col-test","name":"Robot Mixed Collision Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* allCollisionParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new AllCollisionRobotDescriptionParser(); },
      R"({"id":"robot-all-col-test","name":"Robot All Collision Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* unresolvedMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new UnresolvedMeshRobotDescriptionParser(); },
      R"({"id":"robot-mesh-test","name":"Robot Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* resolvedMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new ResolvedMeshRobotDescriptionParser(); },
      R"({"id":"robot-resolved-mesh-test","name":"Robot Resolved Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

const PJ_message_parser_vtable_t* colladaMeshParserVtable() {
  static const PJ_message_parser_vtable_t* vt = PJ::MessageParserPluginBase::vtableWithCreate(
      []() noexcept -> void* { return new CountingColladaUrdfParser(); },
      R"({"id":"robot-collada-mesh-test","name":"Robot Collada Mesh Test","version":"1.0.0","encoding":"test"})");
  return vt;
}

PJ::ObjectTopicId registerTopic(PJ::SessionManager& session, std::string topic_name = "/robot_description") {
  auto topic = session.objectStore().registerTopic(
      PJ::ObjectTopicDescriptor{.dataset_id = 1, .topic_name = std::move(topic_name), .metadata_json = "{}"});
  EXPECT_TRUE(topic.has_value());
  return *topic;
}

void registerParser(PJ::SessionManager& session, PJ::ObjectTopicId topic_id, const PJ_message_parser_vtable_t* vtable) {
  auto parser = std::make_unique<PJ::MessageParserHandle>(vtable);
  ASSERT_TRUE(parser->bindSchema("robot_description", PJ::Span<const uint8_t>{}));
  session.registerObjectTopicParser(topic_id, std::move(parser));
}

pj::scene3d::Scene3DLayerContext makeContext(PJ::SessionManager& session) {
  pj::scene3d::Scene3DLayerContext ctx;
  ctx.session = &session;
  ctx.tf_buffer = std::make_shared<pj::scene3d::TransformBuffer>();
  return ctx;
}

void pushWireBytes(PJ::SessionManager& session, PJ::ObjectTopicId topic_id) {
  ASSERT_TRUE(session.objectStore().pushOwned(topic_id, 123, std::vector<uint8_t>{0x00, 0x04, 0xFF, 0x10}));
}

}  // namespace

TEST(RobotModelLayerTest, TopicSourceDecodesThroughParserAndAppliesFramePrefix) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  layer.setFramePrefix(u"robot1/"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.robotModel()->root_link, "base_link");
  EXPECT_EQ(layer.sourceFrame(), u"robot1/base_link"_s);
  EXPECT_EQ(layer.linkFrameName("tool0"), u"robot1/tool0"_s);
  EXPECT_TRUE(layer.fallbackFrames().contains("robot1/base_link"_L1));
}

TEST(RobotModelLayerTest, TimeRangeIsInvertedEvenWhenLatchIsMissing) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  const auto range = layer.timeRange();
  EXPECT_EQ(range.min, PJ::Timepoint::max());
  EXPECT_EQ(range.max, PJ::Timepoint::min());
  EXPECT_GT(range.min, range.max);
  EXPECT_TRUE(layer.statusText().contains("Waiting for /robot_description"_L1));
}

TEST(RobotModelLayerTest, FormatOtherThanUrdfIsRejectedWithStatus) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, sdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(layer.robotModel(), nullptr);
  EXPECT_TRUE(layer.statusText().contains("Format 'sdf' is not supported"_L1));
}

TEST(RobotModelLayerTest, UnresolvedPackageMeshIsCountedForPlaceholderRendering) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, unresolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.totalMeshCount(), 1);
  EXPECT_EQ(layer.unresolvedMeshCount(), 1);
  EXPECT_TRUE(layer.statusText().contains("packages unresolved"_L1));
}

// M.48: render() memoizes the per-link DrawCall lists and rebuilds them only
// when their inputs change. This asserts the geometry invalidation set: every
// geometry-affecting call must re-flag the cache. The render-origin dependency
// introduced by camera-relative rendering is covered separately below.
// render() itself (which clears the flag) needs a GL context the harness lacks,
// so we clear the flag manually via the test hook before each call.
TEST(RobotModelLayerTest, DrawCacheInvalidationSetIsExhaustive) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  // attach() -> loadFromCurrentSource()/applyRobotDescription() leave the cache
  // dirty so the first render() builds it.
  EXPECT_TRUE(layer.drawsDirtyForTest());

  // Each geometry-affecting call must re-set the flag after a (simulated) render.
  const auto expect_dirties = [&](const char* what, auto&& mutate) {
    layer.clearDrawsDirtyForTest();
    mutate();
    EXPECT_TRUE(layer.drawsDirtyForTest()) << what << " did not invalidate the draw cache";
  };

  expect_dirties("setTrackerTime", [&]() { layer.setTrackerTime(PJ::fromRaw(123)); });
  expect_dirties("setFixedFrame", [&]() { layer.setFixedFrame(u"map"_s); });
  expect_dirties("setFramePrefix", [&]() { layer.setFramePrefix(u"robotA/"_s); });
  expect_dirties("setDisplayMode", [&]() { layer.setDisplayMode(pj::scene3d::RobotModelLayer::DisplayMode::kVisual); });
  expect_dirties("setFallbackColor", [&]() { layer.setFallbackColor(QColor(10, 20, 30)); });

  // setVisible(false) must NOT touch the flag (the cache is simply not consumed
  // while hidden); setVisible(true) must, since TF may have advanced meanwhile.
  layer.clearDrawsDirtyForTest();
  layer.setVisible(false);
  EXPECT_FALSE(layer.drawsDirtyForTest()) << "hiding the layer should not dirty the cache";
  layer.setVisible(true);
  EXPECT_TRUE(layer.drawsDirtyForTest()) << "un-hiding the layer must dirty the cache";

  expect_dirties("detach", [&]() { layer.detach(); });
}

// Regression: a finished async URDF mesh load must itself request the repaint
// that consumes it (QFutureWatcher -> pollMeshLoads). The app paints strictly on
// demand, so before the fix the result was only drained inside render() and the
// magenta placeholder lingered until the user scrubbed. After attach() kicks the
// load, only the event loop is pumped here — no render() or setTrackerTime() —
// and repaintRequested must still fire.
TEST(RobotModelLayerTest, MeshLoadCompletionRequestsRepaintWithoutRender) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, resolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));  // parses the URDF and kicks the fixture mesh load
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.totalMeshCount(), 1);
  ASSERT_EQ(layer.unresolvedMeshCount(), 0) << "fixture mesh must resolve or no load is kicked";

  // Connect AFTER attach so synchronous emissions during attach can't count.
  int repaints = 0;
  QObject::connect(&layer, &pj::scene3d::RobotModelLayer::repaintRequested, [&repaints] { ++repaints; });

  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "finished mesh load did not request a repaint (only render() would have consumed it)";
}

// M.28 regression: the "Ignore COLLADA up_axis" toggle must actually reload the
// model (the flip is baked into the loaded MeshData, so a bare repaint would do
// nothing). Toggling setIgnoreColladaUpAxis on a .dae-mesh URDF re-decodes the
// source and re-kicks the mesh load; an unchanged value is a no-op.
TEST(RobotModelLayerTest, ColladaUpAxisToggleReloadsModel) {
  g_collada_urdf_parser_calls.store(0);

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, colladaMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.totalMeshCount(), 1);
  ASSERT_EQ(layer.unresolvedMeshCount(), 0) << "the .dae fixture mesh must resolve";
  const int decodes_after_attach = g_collada_urdf_parser_calls.load();
  ASSERT_GE(decodes_after_attach, 1);

  // Toggling the flag must re-decode the source (loadFromCurrentSource), so the
  // .dae re-imports under the new effective flip.
  layer.setIgnoreColladaUpAxis(true);
  EXPECT_GT(g_collada_urdf_parser_calls.load(), decodes_after_attach)
      << "setIgnoreColladaUpAxis did not reload the model";
  EXPECT_NE(layer.robotModel(), nullptr) << "model dropped after the up-axis toggle";
  EXPECT_EQ(layer.totalMeshCount(), 1);

  // Setting the same value again is a no-op (no extra decode).
  const int decodes_after_toggle = g_collada_urdf_parser_calls.load();
  layer.setIgnoreColladaUpAxis(true);
  EXPECT_EQ(g_collada_urdf_parser_calls.load(), decodes_after_toggle)
      << "an unchanged setIgnoreColladaUpAxis must not reload";

  // Drain the kicked mesh load so its watcher fires (and is severed) before
  // teardown, then assert the reloaded mesh actually loaded: the watcher-driven
  // pollMeshLoads recomputes loaded_mesh_count_ to total via updateMeshCounters.
  int repaints = 0;
  QObject::connect(
      &layer, &pj::scene3d::RobotModelLayer::meshLoadStatusChanged,
      [&repaints](int, int, const QStringList&) { ++repaints; });
  QElapsedTimer timer;
  timer.start();
  while (repaints == 0 && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  EXPECT_GT(repaints, 0) << "reloaded .dae mesh load never completed (no meshLoadStatusChanged)";
}

// Regression for the file-reload UAF (PR #175 pattern): SessionManager::
// replaceDataset() re-registers each surviving topic's parser slot, destroying
// the previous MessageParserHandle. A layer that cached the raw parser pointer
// at attach() then decodes through freed memory on the next re-decode.
//
// The robot layer's risk is the latch-retry path: attach() on an empty topic
// arms latch_pending_, and every setTrackerTime() while pending re-invokes
// tryLoadTopicDescription(). With a cached parser_ that decode reaches the freed
// handle after a reload; resolving the binding per use rebinds it to the new
// parser. This test drives exactly that path (attach empty -> reload -> push ->
// tick past the 500ms retry interval) so it FAILS on the cached-pointer code and
// passes once the binding is resolved per use.
TEST(RobotModelLayerTest, ReloadSwapsParserWithoutTouchingStaleOne) {
  g_first_urdf_parser_calls.store(0);
  g_second_urdf_parser_calls.store(0);

  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, countingUrdfVtable([]() noexcept -> void* {
                   return new CountingUrdfParser(&g_first_urdf_parser_calls);
                 }));
  // NOTE: no payload yet — attach must latch and wait for the first sample.

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_EQ(layer.robotModel(), nullptr);  // nothing to decode yet -> latched
  EXPECT_EQ(g_first_urdf_parser_calls.load(), 0);

  // Keep the first parser's memory readable from the test so the buggy
  // stale-pointer call below is a deterministic wrong-parser hit rather than UB.
  // Production holds no such guard — there the same call is a use-after-free.
  const auto stale_guard = session.parserKeepaliveForObjectTopic(topic_id);
  ASSERT_NE(stale_guard, nullptr);

  // Simulate the same-file reload: re-register the surviving topic's parser under
  // its stable id, overwriting the slot and dropping the old handle. Then the new
  // generation's first sample arrives.
  registerParser(session, topic_id, countingUrdfVtable([]() noexcept -> void* {
                   return new CountingUrdfParser(&g_second_urdf_parser_calls);
                 }));
  pushWireBytes(session, topic_id);

  // The latch retry is rate-limited to 500ms; wait it out so the next tick fires
  // tryLoadTopicDescription() (and pump the event loop in case anything posted).
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 600) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  layer.setTrackerTime(PJ::fromRaw(123));  // pending + interval elapsed -> retry decode

  EXPECT_EQ(g_first_urdf_parser_calls.load(), 0)
      << "layer called the replaced (freed-in-production) parser after the reload swap";
  EXPECT_GE(g_second_urdf_parser_calls.load(), 1) << "layer did not rebind to the re-registered parser";
  EXPECT_NE(layer.robotModel(), nullptr) << "latch retry did not decode the new sample";
}

// M.49/H.14 regression: the kUrl source must not block the caller. setSourceUrl
// (and therefore attach()/xmlLoadState restores and undo/redo) only kicks the
// fetch off — nothing is applied synchronously — and the model lands later
// through the event loop. The old code spun a nested QEventLoop here, which
// both froze the UI for up to 15s and allowed re-entrant layer destruction.
TEST(RobotModelLayerTest, UrlSourceLoadsAsynchronouslyFromFileUrl) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString urdf_path = dir.filePath(u"decoded.urdf"_s);
  {
    QFile file(urdf_path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(kDecodedUrdf);
  }

  pj::scene3d::RobotModelLayer layer(PJ::ObjectTopicId{.id = 1}, u"robot"_s);
  layer.setSourceUrl(QUrl::fromLocalFile(urdf_path).toString());

  // Asynchronous kickoff: nothing may have been applied before returning.
  EXPECT_EQ(layer.robotModel(), nullptr) << "kUrl load applied synchronously (blocking-fetch regression)";
  EXPECT_TRUE(layer.statusText().contains("Fetching"_L1)) << layer.statusText().toStdString();

  QElapsedTimer timer;
  timer.start();
  while (layer.robotModel() == nullptr && timer.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  ASSERT_NE(layer.robotModel(), nullptr) << "async URL fetch never applied the model; status: "
                                         << layer.statusText().toStdString();
  EXPECT_EQ(layer.robotModel()->root_link, "base_link");
  EXPECT_FALSE(layer.statusText().contains("Fetching"_L1));
}

// M.27: a kTopic robot layer must persist enough identity (dataset + topic name)
// that xmlLoadState re-resolves the same topic and re-decodes its model. The
// round trip must survive a fresh layer wired to a different default topic id.
TEST(RobotModelLayerTest, TopicSourceIdentitySurvivesSaveLoadRoundTrip) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session, "/robot_description");
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer source_layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(source_layer.attach(ctx));
  ASSERT_NE(source_layer.robotModel(), nullptr);

  QDomDocument doc;
  const QDomElement saved = source_layer.xmlSaveState(doc);

  // A second topic with a different id stands in for "the constructor default no
  // longer points at /robot_description" — the restore must re-resolve by name.
  // It has its own (SDF) parser, so a layer left on this default would decode the
  // WRONG, unsupported model; only re-resolution by name yields the URDF.
  const PJ::ObjectTopicId other_id = registerTopic(session, "/other_description");
  ASSERT_NE(other_id.id, topic_id.id);
  registerParser(session, other_id, sdfParserVtable());
  pushWireBytes(session, other_id);

  pj::scene3d::RobotModelLayer restored_layer(other_id, u"/other_description"_s);
  ASSERT_TRUE(restored_layer.attach(ctx));
  ASSERT_TRUE(restored_layer.xmlLoadState(saved));
  ASSERT_NE(restored_layer.robotModel(), nullptr) << "restored layer did not re-resolve the persisted source topic";
  EXPECT_EQ(restored_layer.robotModel()->root_link, "base_link");
}

// Seed "scene -> child" (identity) into a buffer.
void seedSceneChild(pj::scene3d::TransformBuffer& buf, const std::string& child) {
  pj::scene3d::StampedTransform tf;
  tf.stamp = PJ::fromRaw(1000);
  tf.parent_frame = "scene";
  tf.child_frame = child;
  tf.transform = pj::scene3d::Transform{glm::dvec3(0, 0, 0), glm::dquat(1, 0, 0, 0)};
  ASSERT_TRUE(buf.setTransform(tf).has_value());
}

// The DROID gripper bug: the layer must bridge the URDF's fixed joints into the
// buffer it actually RENDERS against (frame_ctx.tf — the dock's live buffer), not
// the ctx_ snapshot captured at attach (which can be null/stale). Drive the draw
// rebuild with a live buffer distinct from the attach-time context to prove the
// render buffer is the one bridged.
TEST(RobotModelLayerTest, FixedJointBridgeReconnectsChildFrameInRenderBuffer) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, bridgeParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);  // attach-time context (its buffer is NOT the one we render against)
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  ASSERT_EQ(layer.robotModel()->joints.size(), 1u);

  // The live render buffer the dock feeds via frame_ctx — a distinct object that
  // publishes base_link under scene but never the fixed-joint child gripper_base.
  pj::scene3d::TransformBuffer live;
  seedSceneChild(live, "base_link");
  EXPECT_FALSE(live.tryLookupTransform("scene", "gripper_base", PJ::fromRaw(2000)).has_value());

  const std::string fixed_frame = "scene";
  const pj::scene3d::FrameContext frame_ctx{live, fixed_frame, PJ::fromRaw(2000)};
  layer.rebuildDrawCacheForTest(frame_ctx);

  const auto tf = live.tryLookupTransform("scene", "gripper_base", PJ::fromRaw(2000));
  ASSERT_TRUE(tf.has_value()) << "fixed-joint mount must be bridged into the LIVE render buffer";
  EXPECT_NEAR(tf->t.z, 0.1, 1e-9);
}

// The bridge frames must carry the layer's frame prefix, or they would not match
// the prefixed names the link poses are looked up under.
TEST(RobotModelLayerTest, FixedJointBridgeAppliesFramePrefix) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, bridgeParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  layer.setFramePrefix(u"robot1/"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::scene3d::TransformBuffer live;
  seedSceneChild(live, "robot1/base_link");

  const std::string fixed_frame = "scene";
  const pj::scene3d::FrameContext frame_ctx{live, fixed_frame, PJ::fromRaw(2000)};
  layer.rebuildDrawCacheForTest(frame_ctx);

  const auto tf = live.tryLookupTransform("scene", "robot1/gripper_base", PJ::fromRaw(2000));
  ASSERT_TRUE(tf.has_value()) << "prefixed bridge frame should resolve in the render buffer";
  EXPECT_NEAR(tf->t.z, 0.1, 1e-9);
}

// Camera-relative rendering made RobotModelLayer's cached DrawCall matrices
// render-origin dependent: FrameContext::lookup subtracts the camera focal before
// the float downcast. A stopped-playback camera pan/zoom changes that origin
// without changing tracker time, so the cache must rebuild or URDF meshes stay in
// the old render space while pointclouds/TF gizmos use the new one.
TEST(RobotModelLayerTest, DrawCacheInvalidatesWhenRenderOriginChanges) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::scene3d::TransformBuffer live;
  seedSceneChild(live, "base_link");

  const std::string fixed_frame = "scene";
  const pj::scene3d::FrameContext origin_zero{live, fixed_frame, PJ::fromRaw(2000), glm::dvec3{0.0, 0.0, 0.0}};
  layer.rebuildDrawCacheForTest(origin_zero);
  layer.clearDrawsDirtyForTest();
  ASSERT_EQ(layer.visualDrawCountForTest(), 1);
  EXPECT_FALSE(layer.drawCacheNeedsRebuildForTest(origin_zero));
  EXPECT_NEAR(layer.firstVisualDrawTranslationForTest().x, 0.0f, 1e-5f);

  const pj::scene3d::FrameContext origin_shifted{live, fixed_frame, PJ::fromRaw(2000), glm::dvec3{3.0, 0.0, 0.0}};
  EXPECT_TRUE(layer.drawCacheNeedsRebuildForTest(origin_shifted))
      << "render-origin changes must invalidate render-space mesh matrices";

  layer.rebuildDrawCacheForTest(origin_shifted);
  layer.clearDrawsDirtyForTest();
  EXPECT_FALSE(layer.drawCacheNeedsRebuildForTest(origin_shifted));
  EXPECT_NEAR(layer.firstVisualDrawTranslationForTest().x, -3.0f, 1e-5f)
      << "draw cache should be rebuilt in the new camera-relative space";
}

// kAuto in a MIXED model: a collision-only sublink (the self-collision capsules)
// must land in the COLLISION draw group so the Collision toggle/opacity hides it —
// NOT be promoted into the visuals group where only the Meshes toggle reaches it.
TEST(RobotModelLayerTest, AutoModeCollisionOnlyLinkRendersInCollisionGroupWhenModelHasVisuals) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, mixedCollisionParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);  // kAuto default
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::scene3d::TransformBuffer live;
  seedSceneChild(live, "body");  // the fixed joint body->body_sc is bridged by the rebuild

  const std::string fixed_frame = "scene";
  const pj::scene3d::FrameContext frame_ctx{live, fixed_frame, PJ::fromRaw(2000)};
  layer.rebuildDrawCacheForTest(frame_ctx);

  EXPECT_EQ(layer.visualDrawCountForTest(), 1) << "the body's <visual> box";
  EXPECT_EQ(layer.collisionDrawCountForTest(), 1) << "body_sc collision sphere must be in the collision group";

  // Collision geometry with no <material> gets an orange tint (RViz convention).
  const glm::vec4 color = layer.firstCollisionDrawColorForTest();
  EXPECT_NEAR(color.r, 1.0f, 1e-3);
  EXPECT_NEAR(color.g, 0.5f, 1e-3);
  EXPECT_NEAR(color.b, 0.1f, 1e-3);
}

// kAuto in an ENTIRELY-collision model: promote to the visuals group so it renders
// solid instead of ghosting at the collision opacity (review L.21 preserved).
TEST(RobotModelLayerTest, AutoModeAllCollisionModelPromotesToVisuals) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, allCollisionParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, u"/robot_description"_s);  // kAuto default
  const auto ctx = makeContext(session);
  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);

  pj::scene3d::TransformBuffer live;
  seedSceneChild(live, "solid");

  const std::string fixed_frame = "scene";
  const pj::scene3d::FrameContext frame_ctx{live, fixed_frame, PJ::fromRaw(2000)};
  layer.rebuildDrawCacheForTest(frame_ctx);

  EXPECT_EQ(layer.visualDrawCountForTest(), 1) << "collision-only model promoted to visuals";
  EXPECT_EQ(layer.collisionDrawCountForTest(), 0);
}

// Custom main: QFutureWatcher/UrlFetcher tests need an event loop, and the
// QCoreApplication must die BEFORE exit handlers run — QtNetwork (loaded by the
// layer's UrlFetcher) registers global cleanup that a function-local-static app
// would outlive, crashing at exit (pj_marketplace's download_manager_test pattern).
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
