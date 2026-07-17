#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Test support: the single home of the platform DSO-suffix switch shared by the
// pj_runtime test binaries ("ds" -> "ds.so" / "ds.dll" / "ds.dylib").

#include <string>
#include <string_view>

namespace PJ::test {

inline std::string pluginFileName(std::string_view stem) {
#if defined(_WIN32)
  return std::string(stem) + ".dll";
#elif defined(__APPLE__)
  return std::string(stem) + ".dylib";
#else
  return std::string(stem) + ".so";
#endif
}

}  // namespace PJ::test
