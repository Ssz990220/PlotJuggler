#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Shared vtable boilerplate for the mock DataSource plugins used by the
// runtime-catalog tests: every mock is the same inert stub, differing only in
// its manifest JSON. makeMockDataSourceVtable() bakes a manifest into a
// vtable; each mock DSO wraps it in PJ_get_data_source_vtable(), and
// in-process tests (static registration) can hand it to registerStatic*
// directly. The manifest_json pointer is stored, not copied — pass a string
// literal (or anything that outlives the vtable).

#include "pj_base/data_source_protocol.h"

namespace pj_mock {
namespace detail {

inline void* create() noexcept {
  return reinterpret_cast<void*>(0x1);
}

// A broken create() for tests of the host's instance-validity guard.
inline void* createNull() noexcept {
  return nullptr;
}

inline void destroy(void*) noexcept {}

inline uint64_t capabilities(void*) noexcept {
  return 0;
}

inline bool bind(void*, PJ_service_registry_t, PJ_error_t*) noexcept {
  return true;
}

inline bool save(void*, PJ_string_view_t* out_json, PJ_error_t*) noexcept {
  static constexpr const char* kJson = "{}";
  if (out_json != nullptr) {
    out_json->data = kJson;
    out_json->size = 2;
  }
  return true;
}

inline bool load(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

inline bool ok(void*, PJ_error_t*) noexcept {
  return true;
}

inline void stop(void*) noexcept {}

inline PJ_data_source_state_t state(void*) noexcept {
  return PJ_DATA_SOURCE_STATE_IDLE;
}

inline PJ_borrowed_dialog_t dialog(void*) noexcept {
  return PJ_borrowed_dialog_t{nullptr, nullptr};
}

inline const void* extension(void*, PJ_string_view_t) noexcept {
  return nullptr;
}

}  // namespace detail

// create_fn is overridable so a test can model a plugin whose create() fails.
inline PJ_data_source_vtable_t makeMockDataSourceVtable(
    const char* manifest_json, void* (*create_fn)() noexcept = detail::create) noexcept {
  return PJ_data_source_vtable_t{
      PJ_DATA_SOURCE_PROTOCOL_VERSION,
      sizeof(PJ_data_source_vtable_t),
      create_fn,
      detail::destroy,
      manifest_json,
      detail::capabilities,
      detail::bind,
      detail::save,
      detail::load,
      detail::ok,
      detail::stop,
      detail::ok,
      detail::ok,
      detail::ok,
      detail::state,
      detail::dialog,
      detail::extension,
  };
}

}  // namespace pj_mock
