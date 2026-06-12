// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/layers/robot_model_layer.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pj_plugins/host/message_parser_handle.hpp"
#include "pj_plugins/sdk/message_parser_plugin_base.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"
#include "pj_scene3d_core/tf/tf_buffer.h"

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

// QFutureWatcher delivers finished() through the event loop, so tests that wait
// on mesh-load completion need a QCoreApplication (created once, lazily).
void ensureCoreApplication() {
  static int argc = 1;
  static char arg0[] = "robot_model_layer_test";
  static char* argv[] = {arg0, nullptr};
  static QCoreApplication app(argc, argv);
  Q_UNUSED(app);
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

  pj::scene3d::RobotModelLayer layer(topic_id, QStringLiteral("/robot_description"));
  layer.setFramePrefix(QStringLiteral("robot1/"));
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.robotModel()->root_link, "base_link");
  EXPECT_EQ(layer.sourceFrame(), QStringLiteral("robot1/base_link"));
  EXPECT_EQ(layer.linkFrameName("tool0"), QStringLiteral("robot1/tool0"));
  EXPECT_TRUE(layer.fallbackFrames().contains(QStringLiteral("robot1/base_link")));
}

TEST(RobotModelLayerTest, TimeRangeIsInvertedEvenWhenLatchIsMissing) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, urdfParserVtable());

  pj::scene3d::RobotModelLayer layer(topic_id, QStringLiteral("/robot_description"));
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  const auto range = layer.timeRange();
  EXPECT_EQ(range.min, PJ::Timepoint::max());
  EXPECT_EQ(range.max, PJ::Timepoint::min());
  EXPECT_GT(range.min, range.max);
  EXPECT_TRUE(layer.statusText().contains(QStringLiteral("Waiting for /robot_description")));
}

TEST(RobotModelLayerTest, FormatOtherThanUrdfIsRejectedWithStatus) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, sdfParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, QStringLiteral("/robot_description"));
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  EXPECT_EQ(layer.robotModel(), nullptr);
  EXPECT_TRUE(layer.statusText().contains(QStringLiteral("Format 'sdf' is not supported")));
}

TEST(RobotModelLayerTest, UnresolvedPackageMeshIsCountedForPlaceholderRendering) {
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, unresolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, QStringLiteral("/robot_description"));
  const auto ctx = makeContext(session);

  ASSERT_TRUE(layer.attach(ctx));
  ASSERT_NE(layer.robotModel(), nullptr);
  EXPECT_EQ(layer.totalMeshCount(), 1);
  EXPECT_EQ(layer.unresolvedMeshCount(), 1);
  EXPECT_TRUE(layer.statusText().contains(QStringLiteral("packages unresolved")));
}

// Regression: a finished async URDF mesh load must itself request the repaint
// that consumes it (QFutureWatcher -> pollMeshLoads). The app paints strictly on
// demand, so before the fix the result was only drained inside render() and the
// magenta placeholder lingered until the user scrubbed. After attach() kicks the
// load, only the event loop is pumped here — no render() or setTrackerTime() —
// and repaintRequested must still fire.
TEST(RobotModelLayerTest, MeshLoadCompletionRequestsRepaintWithoutRender) {
  ensureCoreApplication();
  PJ::SessionManager session;
  const PJ::ObjectTopicId topic_id = registerTopic(session);
  registerParser(session, topic_id, resolvedMeshParserVtable());
  pushWireBytes(session, topic_id);

  pj::scene3d::RobotModelLayer layer(topic_id, QStringLiteral("/robot_description"));
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
