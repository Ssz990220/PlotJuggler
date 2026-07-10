// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/plugin_data_api.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_base/sdk/settings_store_host.hpp"
#include "pj_base/sdk/toolbox_plugin_base.hpp"
#include "pj_base/toolbox_protocol.h"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/ExtensionCatalogService.h"
#include "pj_runtime/ToolboxRuntimeHost.h"
using namespace Qt::StringLiterals;

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace {

class ToolboxRuntimeHostTest : public ::testing::Test {
 protected:
  PJ::sdk::ServiceRegistry registered() {
    host_->registerServices(builder_);
    return PJ::sdk::ServiceRegistry(builder_.view());
  }

  // Rows become visible only after a flush; createDataSource's handle id is the
  // backing DatasetId (see DatastoreToolboxHost), so we read back through it.
  [[nodiscard]] uint64_t totalRowCount(uint32_t source_id) const {
    PJ::DataReader reader(engine_);
    const auto topics = reader.listTopics(static_cast<PJ::DatasetId>(source_id));
    if (topics.empty()) {
      return 0;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? metadata->total_row_count : 0;
  }

  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::sdk::InMemorySettingsBackend settings_;
  PJ::ServiceRegistryBuilder builder_;
  std::unique_ptr<PJ::ToolboxRuntimeHost> host_;
};

TEST_F(ToolboxRuntimeHostTest, RegistersWriteRuntimeAndSettingsServices) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  auto services = registered();

  EXPECT_TRUE(services.get<PJ::sdk::ToolboxHostService>().has_value());
  EXPECT_TRUE(services.get<PJ::sdk::ToolboxRuntimeHostService>().has_value());
  EXPECT_TRUE(services.get<PJ::sdk::SettingsStoreService>().has_value());
}

TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedFiresOnDataChangedCallback) {
  int calls = 0;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&calls](std::vector<PJ::DatasetId>) { ++calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  runtime->notifyDataChanged();
  EXPECT_EQ(calls, 1);
}

TEST_F(ToolboxRuntimeHostTest, ReportMessageRoutesLevelAndTextToOnMessage) {
  std::vector<std::pair<PJ_toolbox_message_level_t, std::string>> received;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_message = [&received](PJ_toolbox_message_level_t level, std::string text) {
    received.emplace_back(level, std::move(text));
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  runtime->reportMessage(PJ::ToolboxMessageLevel::kError, "fetch failed");
  ASSERT_EQ(received.size(), 1U);
  EXPECT_EQ(received[0].first, PJ_TOOLBOX_MESSAGE_ERROR);
  EXPECT_EQ(received[0].second, "fetch failed");
}

// The runtime-host vtable advertises notify_data_changed as [thread-safe]: a
// plugin worker thread may call it. The host must not run the (Qt-touching)
// callback on that worker thread — it marshals it to the constructing/GUI
// thread. Lock the contract: fire from a std::thread, prove the callback did
// NOT run synchronously on the worker, then runs on the host thread once its
// event loop is pumped.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedMarshalsCallbackToConstructingThread) {
  std::atomic<bool> fired{false};
  std::thread::id callback_thread;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&](std::vector<PJ::DatasetId>) {
    callback_thread = std::this_thread::get_id();
    fired.store(true);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto host_thread = std::this_thread::get_id();
  std::thread worker([&]() { runtime->notifyDataChanged(); });
  worker.join();

  // Cross-thread => queued: the callback must not have run on the worker.
  EXPECT_FALSE(fired.load());

  QCoreApplication::processEvents();

  EXPECT_TRUE(fired.load());
  EXPECT_EQ(callback_thread, host_thread);
}

TEST_F(ToolboxRuntimeHostTest, ReportMessageMarshalsCallbackToConstructingThread) {
  std::atomic<bool> fired{false};
  std::thread::id callback_thread;
  std::string received_text;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_message = [&](PJ_toolbox_message_level_t, std::string text) {
    callback_thread = std::this_thread::get_id();
    received_text = std::move(text);
    fired.store(true);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto host_thread = std::this_thread::get_id();
  std::thread worker([&]() { runtime->reportMessage(PJ::ToolboxMessageLevel::kWarning, "from worker"); });
  worker.join();

  EXPECT_FALSE(fired.load());

  QCoreApplication::processEvents();

  EXPECT_TRUE(fired.load());
  EXPECT_EQ(callback_thread, host_thread);
  EXPECT_EQ(received_text, "from worker");
}

// End-to-end write -> notify -> read: a plugin writes through ToolboxHostService
// (buffered), and notifyDataChanged must seal those writes (flushPending) before
// firing on_data_changed, so the freshly written rows are visible to a reader by
// the time the host rebuilds its catalog.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedFlushesBufferedWritesBeforeCatalogRebuild) {
  std::atomic<int> data_changed_calls{0};
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&](std::vector<PJ::DatasetId>) { ++data_changed_calls; };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, std::move(callbacks));
  auto services = registered();

  auto toolbox = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox.has_value()) << toolbox.error();
  auto runtime = services.get<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime.has_value());

  const auto source = *toolbox->createDataSource("mosaico");
  const auto topic = *toolbox->ensureTopic(source, "imu");
  ASSERT_TRUE(toolbox->ensureField(topic, "ax", PJ::PrimitiveType::kFloat64).has_value());
  const std::vector<PJ::sdk::NamedFieldValue> row = {{.name = "ax", .value = 1.5}};
  ASSERT_TRUE(toolbox->appendRecord(topic, 1, row).has_value());

  // Buffered but not flushed: no rows visible yet.
  EXPECT_EQ(totalRowCount(source.id), 0U);

  runtime->notifyDataChanged();

  EXPECT_EQ(data_changed_calls.load(), 1);
  EXPECT_EQ(totalRowCount(source.id), 1U);
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestDelegatesToCatalogParserAndRegistersObjectParser) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog(plugin_file.absolutePath());
  ASSERT_NE(catalog.findParserByEncoding(u"runtime_host_object"_s), nullptr);

  std::vector<PJ::ObjectTopicId> registered_object_parsers;
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  deps.register_object_parser = [&registered_object_parsers](
                                    PJ::ObjectTopicId id, std::unique_ptr<PJ::MessageParserHandle> parser) {
    EXPECT_NE(parser, nullptr);
    registered_object_parsers.push_back(id);
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();

  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  auto ds = (*toolbox_or).createDataSource("cloud download");
  ASSERT_TRUE(ds.has_value()) << ds.error();

  auto ingest_or = (*runtime_or).createParserIngest(ds->id);
  ASSERT_TRUE(ingest_or.has_value()) << ingest_or.error();
  auto ingest = *ingest_or;

  auto binding = ingest.ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/camera/image",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/image",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(binding.has_value()) << binding.error();

  auto push = ingest.pushMessage(*binding, PJ::Timestamp{100}, []() -> std::vector<uint8_t> { return {1, 2, 3, 4}; });
  ASSERT_TRUE(push.has_value()) << push.error();

  // Release flushes: the stub parser's scalar row (byte_count) becomes readable.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  EXPECT_EQ(totalRowCount(ds->id), 1u);

  // mock/image classifies kImage: one object topic, one stored entry, and the
  // registrar received exactly one render-time parser instance.
  const auto object_topics = object_store_.listTopics(static_cast<PJ::DatasetId>(ds->id));
  ASSERT_EQ(object_topics.size(), 1u);
  EXPECT_EQ(object_store_.entryCount(object_topics.front()), 1u);
  EXPECT_EQ(registered_object_parsers.size(), 1u);

  // Idempotent release; recreate-after-release works.
  EXPECT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  EXPECT_TRUE((*runtime_or).createParserIngest(ds->id).has_value());

  // The recreated context references test-body locals (catalog, registrar):
  // release it and destroy the host while those locals are still alive.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  host_.reset();
}

// notify_data_changed reports WHICH datasets received a parser-ingest context
// since the previous notify, and drains the set: the host uses this to focus
// playback on a bulk import (a 10s cloud snippet must present a 10s timeline),
// while plain write-API notifies keep reporting an empty list.
TEST_F(ToolboxRuntimeHostTest, NotifyDataChangedReportsAndDrainsIngestedDatasets) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;

  std::vector<std::vector<PJ::DatasetId>> reported;
  PJ::ToolboxRuntimeHost::Callbacks callbacks;
  callbacks.on_data_changed = [&reported](std::vector<PJ::DatasetId> ingested) {
    reported.push_back(std::move(ingested));
  };
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, std::move(callbacks), std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());

  // Write-API only: the notify reports no ingested datasets.
  auto plain = (*toolbox_or).createDataSource("write-api toolbox");
  ASSERT_TRUE(plain.has_value());
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_TRUE(reported[0].empty());

  // A parser-ingest context marks its dataset as a bulk import.
  auto ds = (*toolbox_or).createDataSource("cloud download");
  ASSERT_TRUE(ds.has_value());
  ASSERT_TRUE((*runtime_or).createParserIngest(ds->id).has_value());
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds->id).has_value());
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 2u);
  ASSERT_EQ(reported[1].size(), 1u);
  EXPECT_EQ(reported[1][0], static_cast<PJ::DatasetId>(ds->id));

  // Drained: the next notify is back to empty.
  (*runtime_or).notifyDataChanged();
  ASSERT_EQ(reported.size(), 3u);
  EXPECT_TRUE(reported[2].empty());
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestWithoutDepsFailsCleanly) {
  host_ =
      std::make_unique<PJ::ToolboxRuntimeHost>(engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{});
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds = (*toolbox_or).createDataSource("x");
  ASSERT_TRUE(ds.has_value());
  auto ingest = (*runtime_or).createParserIngest(ds->id);
  ASSERT_FALSE(ingest.has_value());
  EXPECT_NE(ingest.error().find("not configured"), std::string::npos);
}

// A context the toolbox never released must still be flushed on host teardown
// so its rows aren't lost.
TEST_F(ToolboxRuntimeHostTest, ParserIngestUnreleasedContextFlushedOnTeardown) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds = (*toolbox_or).createDataSource("teardown flush");
  ASSERT_TRUE(ds.has_value());
  auto ingest_or = (*runtime_or).createParserIngest(ds->id);
  ASSERT_TRUE(ingest_or.has_value());
  auto binding = (*ingest_or)
                     .ensureParserBinding(
                         PJ::ParserBindingRequest{
                             .topic_name = "/scalar",
                             .parser_encoding = "runtime_host_object",
                             .type_name = "mock/scalar",
                             .schema = PJ::Span<const uint8_t>{},
                             .parser_config_json = "{}",
                         });
  ASSERT_TRUE(binding.has_value()) << binding.error();
  ASSERT_TRUE(
      (*ingest_or).pushMessage(*binding, PJ::Timestamp{5}, []() -> std::vector<uint8_t> { return {9}; }).has_value());
  const auto id = ds->id;
  host_.reset();  // NO release — teardown must flush
  EXPECT_EQ(totalRowCount(id), 1u);
}

// Contexts are per-dataset and independent: releasing one leaves the other
// usable, and each dataset ends up with its own rows.
TEST_F(ToolboxRuntimeHostTest, ParserIngestContextsArePerDataset) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto toolbox_or = services.require<PJ::sdk::ToolboxHostService>();
  ASSERT_TRUE(toolbox_or.has_value());
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ds_a = (*toolbox_or).createDataSource("a");
  auto ds_b = (*toolbox_or).createDataSource("b");
  ASSERT_TRUE(ds_a.has_value());
  ASSERT_TRUE(ds_b.has_value());
  ASSERT_NE(ds_a->id, ds_b->id);
  auto ingest_a = (*runtime_or).createParserIngest(ds_a->id);
  auto ingest_b = (*runtime_or).createParserIngest(ds_b->id);
  ASSERT_TRUE(ingest_a.has_value());
  ASSERT_TRUE(ingest_b.has_value());
  auto bind_a = (*ingest_a).ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/a",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(bind_a.has_value());
  ASSERT_TRUE(
      (*ingest_a).pushMessage(*bind_a, PJ::Timestamp{1}, []() -> std::vector<uint8_t> { return {1}; }).has_value());
  // Releasing A leaves B's context usable.
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds_a->id).has_value());
  auto bind_b = (*ingest_b).ensureParserBinding(
      PJ::ParserBindingRequest{
          .topic_name = "/b",
          .parser_encoding = "runtime_host_object",
          .type_name = "mock/scalar",
          .schema = PJ::Span<const uint8_t>{},
          .parser_config_json = "{}",
      });
  ASSERT_TRUE(bind_b.has_value()) << bind_b.error();
  ASSERT_TRUE(
      (*ingest_b).pushMessage(*bind_b, PJ::Timestamp{2}, []() -> std::vector<uint8_t> { return {2}; }).has_value());
  ASSERT_TRUE((*runtime_or).releaseParserIngest(ds_b->id).has_value());
  EXPECT_EQ(totalRowCount(ds_a->id), 1u);
  EXPECT_EQ(totalRowCount(ds_b->id), 1u);
  host_.reset();
}

TEST_F(ToolboxRuntimeHostTest, ParserIngestUnknownDataSourceFails) {
  QFileInfo plugin_file{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog(plugin_file.absolutePath());
  PJ::ToolboxRuntimeHost::ParserIngestDeps deps;
  deps.catalog = &catalog;
  host_ = std::make_unique<PJ::ToolboxRuntimeHost>(
      engine_, object_store_, settings_, PJ::ToolboxRuntimeHost::Callbacks{}, std::move(deps));
  auto services = registered();
  auto runtime_or = services.require<PJ::sdk::ToolboxRuntimeHostService>();
  ASSERT_TRUE(runtime_or.has_value());
  auto ingest = (*runtime_or).createParserIngest(99999);
  ASSERT_FALSE(ingest.has_value());
  EXPECT_NE(ingest.error().find("not found"), std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  QCoreApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
