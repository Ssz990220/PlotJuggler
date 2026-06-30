// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test-only DataSource plugin that exercises the runtime host's message-box
// path from the import worker thread. When its config contains the marker
// "ask_msgbox", importData() calls runtimeHost().askContinue(...) — which the
// host must marshal to the GUI thread before building a QWidget. This mirrors
// what the real CSV loader does on a non-monotonic timestamp, and lets
// file_loader_test assert the host never constructs the QMessageBox off the GUI
// thread (the cross-thread-affinity segfault this guards against).
//
// Claims ".msgboxmock"; writes topic "mock/file_data" with a single row so a
// successful (Continue) askContinue produces a loadable dataset.

#include <pj_base/sdk/data_source_patterns.hpp>
#include <string>

namespace {

class MsgBoxMockSource : public PJ::FileSourceBase {
 public:
  uint64_t extraCapabilities() const override {
    return PJ::kCapabilityDirectIngest;
  }

  std::string saveConfig() const override {
    return config_;
  }

  PJ::Status loadConfig(std::string_view config_json) override {
    config_ = std::string(config_json);
    return PJ::okStatus();
  }

  PJ::Status importData() override {
    // config_ is opaque to this example (no JSON dependency); the marker is
    // matched by substring — safe because only the test sets this config.
    if (config_.find("ask_msgbox") != std::string::npos) {
      // Runs on the import worker thread. The host is contractually required to
      // marshal the dialog to the GUI thread (data_source_protocol.h:
      // show_message_box is [main-thread]); building the QWidget here directly
      // is the bug under test.
      if (!runtimeHost().askContinue("Regression", "Continue the worker-thread load?")) {
        return PJ::unexpected("aborted by user");
      }
    }

    auto topic = writeHost().ensureTopic("mock/file_data");
    if (!topic) {
      return PJ::unexpected(topic.error());
    }
    auto status = writeHost().appendRecord(*topic, PJ::Timestamp{100}, {{.name = "value", .value = 1.0}});
    if (!status) {
      return PJ::unexpected(status.error());
    }
    return PJ::okStatus();
  }

 private:
  std::string config_ = "{}";
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(
    MsgBoxMockSource, R"({"id":"msgbox-mock-source","name":"Msgbox Mock Source",)"
                      R"("version":"1.0.0","description":"Test runtime-host message box from a worker thread",)"
                      R"("file_extensions":[".msgboxmock"]})")
