#include <gtest/gtest.h>

#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/sdk/data_source_host_views.hpp"
#include "pj_base/sdk/service_registry.hpp"
#include "pj_base/sdk/service_traits.hpp"
#include "pj_datastore/engine.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_plugins/host/service_registry_builder.hpp"
#include "pj_runtime/DataSourceRuntimeHost.h"
#include "pj_runtime/ExtensionCatalogService.h"

#ifndef PJ_RUNTIME_HOST_OBJECT_PARSER_PATH
#error "PJ_RUNTIME_HOST_OBJECT_PARSER_PATH must be defined"
#endif

namespace {

class DataSourceRuntimeHostObjectIngestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(catalog_.findParserByEncoding(QStringLiteral("runtime_host_object")), nullptr);

    auto dataset_or = engine_.createDataset(PJ::DatasetDescriptor{.source_name = "test", .time_domain_id = 0});
    ASSERT_TRUE(dataset_or.has_value()) << dataset_or.error();

    dataset_id_ = static_cast<PJ::DatasetId>(*dataset_or);
    source_handle_ = PJ_data_source_handle_t{static_cast<uint32_t>(*dataset_or)};
    host_ = std::make_unique<PJ::DataSourceRuntimeHost>(
        engine_, catalog_, dataset_id_, source_handle_, object_store_, "runtime_host_test_source");
    host_->registerServices(registry_builder_);
  }

  [[nodiscard]] PJ::DataSourceRuntimeHostView runtime() {
    PJ::sdk::ServiceRegistry services(registry_builder_.view());
    auto runtime_or = services.require<PJ::sdk::DataSourceRuntimeHostService>();
    EXPECT_TRUE(runtime_or.has_value()) << runtime_or.error();
    return runtime_or.has_value() ? *runtime_or : PJ::DataSourceRuntimeHostView{};
  }

  [[nodiscard]] PJ::Expected<PJ::ParserBindingHandle> bindTopic(std::string topic_name, std::string type_name) {
    return runtime().ensureParserBinding(
        PJ::ParserBindingRequest{
            .topic_name = topic_name,
            .parser_encoding = "runtime_host_object",
            .type_name = type_name,
            .schema = PJ::Span<const uint8_t>{},
            .parser_config_json = "{}",
        });
  }

  [[nodiscard]] std::shared_ptr<std::atomic<int>> pushPayload(
      PJ::ParserBindingHandle binding, PJ::Timestamp timestamp, const std::vector<uint8_t>& payload) {
    auto fetch_calls = std::make_shared<std::atomic<int>>(0);
    auto status = runtime().pushMessage(binding, timestamp, [payload, fetch_calls]() -> std::vector<uint8_t> {
      fetch_calls->fetch_add(1);
      return payload;
    });
    EXPECT_TRUE(status.has_value()) << status.error();
    return fetch_calls;
  }

  [[nodiscard]] uint64_t totalRowCount() const {
    PJ::DataReader reader(engine_);
    const auto topics = reader.listTopics(dataset_id_);
    if (topics.empty()) {
      return 0;
    }
    const auto metadata = reader.getMetadata(topics.front());
    return metadata.has_value() ? metadata->total_row_count : 0;
  }

  QFileInfo plugin_file_{QString::fromUtf8(PJ_RUNTIME_HOST_OBJECT_PARSER_PATH)};
  PJ::ExtensionCatalogService catalog_{plugin_file_.absolutePath()};
  PJ::DataEngine engine_;
  PJ::ObjectStore object_store_;
  PJ::DatasetId dataset_id_{0};
  PJ_data_source_handle_t source_handle_{};
  std::unique_ptr<PJ::DataSourceRuntimeHost> host_;
  PJ::ServiceRegistryBuilder registry_builder_;
};

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageEagerCommitsScalarsAndStoresOwnedObjectBytes) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kEager);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.descriptor(*object_topic).metadata_json, R"({"builtin_object_type":"kImage"})");
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  // Under always-lazy ingest with captured anchor, object entries go to the
  // store via pushLazy. The lazy slot does not contribute to memoryUsage —
  // bytes are owned upstream via the captured anchor.
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, BindSchemaRegisteredObjectTypeCreatesObjectTopic) {
  auto binding_or = bindTopic("/camera/deferred_image", "mock/bind_schema_image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/deferred_image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.descriptor(*object_topic).metadata_json, R"({"builtin_object_type":"kImage"})");
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageLazyObjectsEagerScalarsCommitsScalarsAndDefersObjectBytes) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kLazyObjectsEagerScalars);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  // The captured-payload closure replays the same PayloadView on every read
  // (it holds onto the upstream anchor) rather than re-invoking the fetcher.
  // So latestAt does NOT trigger an additional fetch.
  EXPECT_EQ(fetch_calls->load(), 1);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessagePureLazyDefersFetchAndDoesNotCommitScalars) {
  host_->policyResolver().setDefault(PJ::sdk::ObjectIngestPolicy::kPureLazy);

  auto binding_or = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 0);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 0U);

  auto object_topic = object_store_.findTopic(dataset_id_, "/camera/image");
  ASSERT_TRUE(object_topic.has_value());
  EXPECT_EQ(object_store_.entryCount(*object_topic), 1U);
  EXPECT_EQ(object_store_.memoryUsage(*object_topic), 0U);
  auto entry = object_store_.latestAt(*object_topic, 123);
  ASSERT_TRUE(entry.has_value());
  ASSERT_NE(entry->payload.anchor, nullptr);
  EXPECT_EQ(std::vector<uint8_t>(entry->payload.bytes.begin(), entry->payload.bytes.end()), payload);
  EXPECT_EQ(fetch_calls->load(), 1);
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, PushMessageKeepsScalarOnlyTopicsEagerUnderLazyPolicy) {
  host_->policyResolver().setForTopic("/scalar/topic", PJ::sdk::ObjectIngestPolicy::kPureLazy);

  auto binding_or = bindTopic("/scalar/topic", "mock/scalar");
  ASSERT_TRUE(binding_or.has_value()) << binding_or.error();

  const std::vector<uint8_t> payload{0x10, 0x20};
  auto fetch_calls = pushPayload(*binding_or, 123, payload);
  EXPECT_EQ(fetch_calls->load(), 1);

  host_->flushAll();
  EXPECT_EQ(totalRowCount(), 1U);
  EXPECT_FALSE(object_store_.findTopic(dataset_id_, "/scalar/topic").has_value());
}

TEST_F(DataSourceRuntimeHostObjectIngestTest, SetObjectRetentionBudgetAppliesToEveryBoundObjectTopic) {
  auto binding_a = bindTopic("/camera/image", "mock/image");
  ASSERT_TRUE(binding_a.has_value()) << binding_a.error();
  auto binding_b = bindTopic("/camera/image_b", "mock/image");
  ASSERT_TRUE(binding_b.has_value()) << binding_b.error();

  constexpr int64_t kWindowNs = 5'000'000'000LL;
  constexpr size_t kMemCap = 16U * 1024U * 1024U;
  host_->setObjectRetentionBudget(kWindowNs, kMemCap);

  for (const auto* name : {"/camera/image", "/camera/image_b"}) {
    auto topic_id = object_store_.findTopic(dataset_id_, name);
    ASSERT_TRUE(topic_id.has_value());
    const auto budget = object_store_.retentionBudget(*topic_id);
    EXPECT_EQ(budget.time_window_ns, kWindowNs);
    EXPECT_EQ(budget.max_memory_bytes, kMemCap);
  }
}

}  // namespace
