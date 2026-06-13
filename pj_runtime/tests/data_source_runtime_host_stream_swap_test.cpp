// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Streaming pause/resume two-engine lockstep — runtime-host INTEGRATION level.
//
// Background. PJ4 streaming double-buffers the DataEngine: live messages land
// in the primary (A); on pause, incoming messages are rerouted into a
// secondary (B) so the frozen view stays stable and bounded; on resume, B
// flushes back into A. The reroute is DataSourceRuntimeHost::setDataEngineTarget.
//
// The bug PR #200 fixes: a streaming plugin registers its topic/fields ONCE
// (data_stream_dummy in onStart(); parser_protobuf on first message) and caches
// the returned TopicHandle / FieldHandle, then reuses them on every subsequent
// sample via appendBoundRecord. A handle is just an integer id into ONE engine.
// After setDataEngineTarget(B) the cached id must resolve to the SAME topic/field
// in B — but on origin/main the source-level write path mirrors NOTHING to the
// secondary, and the parser path mirrors topics but not fields. So the first
// post-pause write fails ("topic N not found" / field lookup fails) and the
// streaming worker dies. That is the reported "data_stream_dummy dies on first
// pause/resume" crash.
//
// These are the runtime-INTEGRATION companion to the datastore-unit tests in
// pj_datastore/tests/streaming_mirror_test.cpp. The unit tests verify the
// createTopicField / setSecondaryEngine MECHANISM in isolation (driving
// DatastoreSourceWriteHost directly). The tests here verify that
// DataSourceRuntimeHost actually WIRES that mechanism — calling
// setSecondaryEngine() on the source write host and on every parser binding —
// and that a plugin's cached handles survive a real setDataEngineTarget swap
// end to end.
//
// They touch only the host's PUBLIC surface (the secondary_data_engine ctor arg
// + setDataEngineTarget), which the fix leaves unchanged, so:
//   * without the fix they FAIL — the host never mirrors source/parser writes
//     to the secondary engine, so the cached handle goes stale across the swap;
//     i.e. they reproduce the reported "data_stream_dummy dies on first
//     pause/resume" bug (source path) and its parser-path variant,
//   * with the fix they PASS, and
//   * they guard the WIRING itself: deleting the host's setSecondaryEngine call
//     re-breaks streaming while the datastore-unit tests stay green.

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/topic_storage.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

#ifndef PJ_STREAMING_CACHING_PARSER_PATH
#error "PJ_STREAMING_CACHING_PARSER_PATH must be defined"
#endif

namespace {

using PJ::PrimitiveType;
using PJ::sdk::BoundFieldValue;
using PJ::sdk::FieldHandle;
using PJ::sdk::SourceWriteHostView;
using PJ::sdk::TopicHandle;

// Two lockstep engines + a DataSourceRuntimeHost wired for streaming
// pause/resume, exercising the SOURCE-LEVEL scalar write path
// (SourceWriteHostService) — the path a plugin like data_stream_dummy uses
// when it caches handles in onStart() and writes bound records per sample.
class StreamEngineSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Primary dataset (id 0 on a fresh engine).
    auto primary_dataset =
        primary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(primary_dataset.has_value()) << primary_dataset.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*primary_dataset);

    // Lockstep dataset on the secondary with the SAME DatasetId — mirrors the
    // real streaming manager, which creates the dataset on both engines so a
    // later topic mirror can address it by id on either side.
    auto secondary_dataset =
        secondary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(secondary_dataset.has_value()) << secondary_dataset.error();
    ASSERT_EQ(static_cast<PJ::DatasetId>(*secondary_dataset), dataset_id_);

    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id_)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        primary_engine_, catalog_, dataset_id_, source_handle_, primary_object_store_, "stream_swap_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/&secondary_engine_,
        /*library_keepalive=*/nullptr);
    host_->registerServices(registry_builder_);
  }

  // The SourceWriteHostView a source plugin would receive from bind().
  [[nodiscard]] SourceWriteHostView sourceWriter() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto writer_or = services.require<PJ::sdk::SourceWriteHostService>();
    EXPECT_TRUE(writer_or.has_value()) << writer_or.error();
    return writer_or.has_value() ? *writer_or : SourceWriteHostView{};
  }

  [[nodiscard]] static uint64_t rowCount(PJ::DataEngine& engine, PJ::TopicId topic_id) {
    PJ::DataReader reader(engine);
    const auto meta = reader.getMetadata(topic_id);
    return meta.has_value() ? meta->total_row_count : 0;
  }

  PJ::ExtensionCatalogService catalog_{QString{}};
  PJ::DataEngine primary_engine_;
  PJ::DataEngine secondary_engine_;
  PJ::ObjectStore primary_object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

// The first ensureTopic/ensureField on the source path must lockstep-mirror the
// new topic + columns onto the secondary engine with the SAME ids — otherwise a
// later setDataEngineTarget(secondary) lands on an engine that has no topic for
// the plugin's cached handle. On origin/main the source write host mirrors
// nothing, so the secondary has no "imu" topic at all → RED here.
TEST_F(StreamEngineSwapTest, FirstEnsureLockstepsTopicAndFieldIdsOntoSecondaryEngine) {
  SourceWriteHostView writer = sourceWriter();

  const auto topic = *writer.ensureTopic("imu");
  const auto field_ax = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  const auto field_ay = *writer.ensureField(topic, "ay", PrimitiveType::kFloat32);

  // FieldIds are dense from 0 — the first field of a topic legitimately gets
  // id 0. (PR #200's std::optional sentinel exists precisely so a forced id 0
  // is not confused with "auto-assign".)
  EXPECT_EQ(field_ax.id, 0U);
  EXPECT_EQ(field_ay.id, 1U);

  ASSERT_TRUE(writer.appendBoundRecord(topic, 10, {{.field = field_ax, .value = 1.0F}}).has_value());
  host_->flushPending();

  // The crux: the secondary engine must hold the same topic id and the same
  // (name, id) columns.
  const auto* sec_storage = secondary_engine_.getTopicStorage(topic.id);
  ASSERT_NE(sec_storage, nullptr) << "secondary engine never received the lockstep topic mirror";
  const auto& cols = sec_storage->columnDescriptors();
  ASSERT_GE(cols.size(), 2U) << "secondary topic is missing the lockstep field columns";
  EXPECT_EQ(cols[0].field_path, "ax");
  EXPECT_EQ(cols[0].field_id, field_ax.id);
  EXPECT_EQ(cols[1].field_path, "ay");
  EXPECT_EQ(cols[1].field_id, field_ay.id);
}

// The reported bug, end to end: cache handles, write to A, pause (swap to B),
// then write THROUGH THE SAME CACHED HANDLES. On origin/main the post-pause
// write fails because B has no matching topic/field → RED. Resume (swap back to
// A) must keep the handles valid too.
TEST_F(StreamEngineSwapTest, CachedSourceHandlesResolveAcrossPauseAndResumeSwap) {
  SourceWriteHostView writer = sourceWriter();

  // Plugin registers + caches its handles once (data_stream_dummy::onStart).
  const auto topic = *writer.ensureTopic("imu");
  const auto field_ax = *writer.ensureField(topic, "ax", PrimitiveType::kFloat32);
  const auto field_ay = *writer.ensureField(topic, "ay", PrimitiveType::kFloat32);

  // Live phase — writes land in the primary.
  ASSERT_TRUE(
      writer.appendBoundRecord(topic, 10, {{.field = field_ax, .value = 1.0F}, {.field = field_ay, .value = 2.0F}})
          .has_value());
  host_->flushPending();

  // Pause: reroute scalar writes to the secondary engine.
  host_->setDataEngineTarget(&secondary_engine_);

  // Write through the cached handles — this is the exact call that died before
  // the fix. The secondary must already know the topic+fields at matching ids.
  const auto paused_write =
      writer.appendBoundRecord(topic, 20, {{.field = field_ax, .value = 3.0F}, {.field = field_ay, .value = 4.0F}});
  ASSERT_TRUE(paused_write.has_value()) << "cached FieldHandle did not resolve on the secondary engine after pause: "
                                        << paused_write.error();
  host_->flushPending();

  // Resume: swap back to the primary. Bidirectional lockstep keeps both valid.
  host_->setDataEngineTarget(&primary_engine_);
  const auto resumed_write =
      writer.appendBoundRecord(topic, 30, {{.field = field_ax, .value = 5.0F}, {.field = field_ay, .value = 6.0F}});
  ASSERT_TRUE(resumed_write.has_value()) << "cached FieldHandle did not resolve on the primary engine after resume: "
                                         << resumed_write.error();
  host_->flushPending();

  // Rows split across the two engines exactly as the pause window dictates:
  // ts 10 + ts 30 on the primary, ts 20 on the secondary.
  EXPECT_EQ(rowCount(primary_engine_, topic.id), 2U);
  EXPECT_EQ(rowCount(secondary_engine_, topic.id), 1U);
}

// Two lockstep engines + a DataSourceRuntimeHost driving the PARSER write path:
// a real parser plugin (streaming_caching_parser_plugin) that caches a
// FieldHandle on its first parse and reuses it — the parser_protobuf pattern.
// PR #200's commit message claims it "closes the latent FieldHandle-stale bug
// for parser plugins (parser_protobuf et al.)", yet ships no parser-path test.
// This fixture is that missing coverage.
class StreamParserSwapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(QStringLiteral("streaming_caching")), nullptr)
        << "streaming_caching_parser_plugin was not discovered in " << plugin_dir_.toStdString();

    auto primary_dataset =
        primary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(primary_dataset.has_value()) << primary_dataset.error();
    dataset_id_ = static_cast<PJ::DatasetId>(*primary_dataset);

    auto secondary_dataset =
        secondary_engine_.createDataset(PJ::DatasetDescriptor{.source_name = "stream", .time_domain_id = 0});
    ASSERT_TRUE(secondary_dataset.has_value()) << secondary_dataset.error();
    ASSERT_EQ(static_cast<PJ::DatasetId>(*secondary_dataset), dataset_id_);

    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(dataset_id_)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        primary_engine_, catalog_, dataset_id_, source_handle_, primary_object_store_, "stream_parser_swap_source",
        /*parser_registrar=*/nullptr, /*secondary_object_store=*/nullptr, /*secondary_data_engine=*/&secondary_engine_,
        /*library_keepalive=*/nullptr);
    host_->registerServices(registry_builder_);
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  // Push one message carrying a float32 payload through the bound parser.
  [[nodiscard]] PJ::Status pushFloat(PJ::ParserBindingHandle binding, PJ::Timestamp timestamp, float value) {
    std::vector<uint8_t> payload(sizeof(float));
    std::memcpy(payload.data(), &value, sizeof(float));
    return runtime().pushMessage(binding, timestamp, [payload]() -> std::vector<uint8_t> { return payload; });
  }

  QFileInfo plugin_file_{QString::fromUtf8(PJ_STREAMING_CACHING_PARSER_PATH)};
  QString plugin_dir_{plugin_file_.absolutePath()};
  PJ::ExtensionCatalogService catalog_{plugin_dir_};
  PJ::DataEngine primary_engine_;
  PJ::DataEngine secondary_engine_;
  PJ::ObjectStore primary_object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

// The parser-path analogue of the source-path repro. The host mirrors the bound
// TOPIC onto the secondary engine, but on origin/main the parser write host
// never mirrors the FIELD. So after the pause swap, the parser's cached
// FieldHandle points at a column the secondary topic does not have, and the
// post-pause push fails → RED. PR #200 wires setSecondaryEngine() onto the
// parser binding's write host, mirroring the field at a matching id → GREEN.
TEST_F(StreamParserSwapTest, CachedParserFieldHandleResolvesAfterPauseSwap) {
  auto binding_or = runtime().ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/imu/value",
          .parser_encoding = "streaming_caching",
          .type_name = "streaming/cached_scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();
  const auto binding = *binding_or;

  // Live phase: first parse caches the FieldHandle and writes to the primary.
  ASSERT_TRUE(pushFloat(binding, 10, 1.0F).has_value());
  host_->flushPending();

  // Pause: reroute parser scalar writes to the secondary engine.
  host_->setDataEngineTarget(&secondary_engine_);

  // The parser reuses its cached FieldHandle. On origin/main the field was
  // never mirrored to the secondary → this push fails.
  const auto paused_push = pushFloat(binding, 20, 2.0F);
  ASSERT_TRUE(paused_push.has_value())
      << "parser's cached FieldHandle did not resolve on the secondary engine after pause: " << paused_push.error();
  host_->flushPending();

  // Resume keeps it valid too.
  host_->setDataEngineTarget(&primary_engine_);
  ASSERT_TRUE(pushFloat(binding, 30, 3.0F).has_value());
  host_->flushPending();
}

}  // namespace
