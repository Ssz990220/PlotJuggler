// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <pj_plugins/dialog_protocol.h>

#include <string>
#include <vector>

/// State shared between the test and the MockPanelPlugin instance.
/// The dialog plugin ABI has no per-instance test parameterization, so the
/// plugin reads/writes from this single global, and tests reset it before
/// each scenario via resetMockPanelState().
struct MockPanelState {
  std::string text;
  std::string label = "Hello";

  bool close_on_next_tick = false;
  std::string close_reason;

  bool sub_dialog_on_next_tick = false;
  std::string sub_dialog_ui;

  std::vector<std::string> events_seen;
};

MockPanelState& mockPanelState();
void resetMockPanelState();
