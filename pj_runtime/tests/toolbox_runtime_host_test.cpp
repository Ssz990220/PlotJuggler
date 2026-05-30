#include <gtest/gtest.h>

#include <QCoreApplication>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include "pj_runtime/ToolboxRuntimeHost.h"

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
  callbacks.on_data_changed = [&calls]() { ++calls; };
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
  callbacks.on_data_changed = [&]() {
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
  callbacks.on_data_changed = [&]() { ++data_changed_calls; };
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

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  QCoreApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
