// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DebugMode.h"

namespace PJ {

namespace {
// Launch-time flag: written once in main() before the main window is built,
// only read afterwards. A plain global is sufficient (no concurrency).
bool g_debug_mode = false;
}  // namespace

void setDebugMode(bool enabled) {
  g_debug_mode = enabled;
}

bool isDebugMode() {
  return g_debug_mode;
}

}  // namespace PJ
