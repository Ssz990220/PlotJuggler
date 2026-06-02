#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <string>
#include <string_view>
#include <vector>

namespace PJ::detail {

// Multi-instance fanout extension: a DataSource plugin's accepted config may
// declare a top-level `__pj_fanout` array (each entry a complete config JSON
// string) to ask the host to spawn N separate plugin instances — one per
// entry, each with its own DatasetId. The doubled-underscore prefix marks the
// key as host-private so it cannot collide with plugin-native fields.
//
// Returns the list of per-instance config strings. If the key is absent or the
// config is not the expected shape, returns `{ config }` so the host runs a
// single-instance import — this preserves back-compat with configs persisted
// before fanout existed. Non-string array entries are skipped (and logged); if
// that leaves nothing usable, the single-instance fallback applies.
std::vector<std::string> extractFanout(std::string_view config);

// Per-fanout entry hint for the dataset display name. The plugin emits this key
// (`display_suffix`) on each fanout config so the host can build a unique
// catalog label like "<basename>/<suffix>" without having to understand the
// plugin's domain. Falls back to `fallback` on any miss.
QString parseDisplaySuffix(std::string_view cfg, const QString& fallback);

// Top-level dataset display-name hint (issue #98). A DataSource plugin emits the
// `display_name` string key in its accepted/saved config to override the
// file-derived catalog/tree root label — the part the host would otherwise take
// from the selected filename (e.g. selecting `info.json` shows `info`). The host
// uses it as the base name (single-instance label, or the prefix combined with
// `display_suffix` in fanout). Sibling of `display_suffix`: plugin-owned, so no
// host-private `__pj_` prefix. Returns an empty QString if the key is absent,
// not a string, or empty — callers then keep the filename default, so plugins
// that don't emit it are unaffected (back-compat).
QString parseDisplayName(std::string_view cfg);

}  // namespace PJ::detail
