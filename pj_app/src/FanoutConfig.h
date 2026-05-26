#pragma once

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

}  // namespace PJ::detail
