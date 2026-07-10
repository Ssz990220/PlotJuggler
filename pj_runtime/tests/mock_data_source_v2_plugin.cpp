// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// A data-source mock sharing the id "mock-data-source" with the SDK's v1
// example fixture, at a higher version (2.0.0) and with no
// min_plotjuggler_version. Used to exercise the version tie-break in the
// runtime catalog's de-duplication.

#include "mock_data_source_vtable.h"

extern "C" PJ_DATA_SOURCE_EXPORT const uint32_t pj_plugin_abi_version = PJ_ABI_VERSION;

extern "C" PJ_DATA_SOURCE_EXPORT const PJ_data_source_vtable_t* PJ_get_data_source_vtable() noexcept {
  static const PJ_data_source_vtable_t vt =
      pj_mock::makeMockDataSourceVtable(R"({"id":"mock-data-source","name":"Mock DataSource","version":"2.0.0"})");
  return &vt;
}
