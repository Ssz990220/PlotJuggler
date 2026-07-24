#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file version_compare.hpp
 * @brief The one version-ordering rule for plugin/extension versions.
 *
 * Shared by the marketplace ("is the installed copy above its bundled
 * version?" — the uninstall lock and downgrade-to-bundled) and by the host's
 * plugin catalog and bundled-plugin seed, so no two layers can ever disagree
 * about which of two versions is newer.
 */

#include <string_view>

namespace PJ {

// Compares two dotted numeric version strings (e.g. "4.1.0" vs "4.0.2"). Only the
// leading numeric components matter: each component's digits are read until the
// first non-digit. A '.' continues to the next component; any other separator
// ('-', '+', …) ends the numeric part, so a pre-release/build suffix is ignored
// *in full* even when it contains dots ("1-rc2", "1-rc.2", "3+meta" all compare as
// "1"/"1"/"3"). A missing trailing component counts as 0 (so "4.1" == "4.1.0").
// Components are compared as unsigned decimals *without* converting to an integer
// type — leading zeros are stripped, then the longer digit run is the larger value,
// else they compare lexicographically. This is overflow-proof: an absurdly long
// component like "999999999999999999999.0.0" is handled correctly, not wrapped.
// Returns <0, 0, or >0 like strcmp.
[[nodiscard]] inline int compareSemver(std::string_view lhs, std::string_view rhs) noexcept {
  // Consume the leading digit run of the current component; returns it with leading
  // zeros stripped ("" == numeric 0). Advance to the next component only across a '.'
  // that immediately follows the digits — any other separator begins a suffix the
  // comparison ignores, so the numeric part ends there. (Scanning for the next '.'
  // instead would wrongly step over a suffix like "-rc.2" and read its digits.)
  auto takeComponent = [](std::string_view& v) -> std::string_view {
    size_t len = 0;
    while (len < v.size() && v[len] >= '0' && v[len] <= '9') {
      ++len;
    }
    const std::string_view run = v.substr(0, len);
    v = (len < v.size() && v[len] == '.') ? v.substr(len + 1) : std::string_view{};
    size_t first_significant = 0;
    while (first_significant < run.size() && run[first_significant] == '0') {
      ++first_significant;
    }
    return run.substr(first_significant);
  };
  while (!lhs.empty() || !rhs.empty()) {
    const std::string_view l = takeComponent(lhs);
    const std::string_view r = takeComponent(rhs);
    if (l.size() != r.size()) {
      return l.size() < r.size() ? -1 : 1;
    }
    if (const int cmp = l.compare(r); cmp != 0) {
      return cmp < 0 ? -1 : 1;
    }
  }
  return 0;
}

}  // namespace PJ
