#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "pj_base/span.hpp"
#include "pj_datastore/column_buffer.hpp"  // PJ::StorageKind
#include "pj_datastore/sample.hpp"

namespace PJ::proc {

/// Scheduling/behaviour traits — drive the engine's eager-vs-lazy placement and
/// the late-after-retention defense (an eager processor must consume every
/// committed sample before retention can evict it).
/// A processor ORs the bits that apply. `TraitMask` is the OR'd result.
using TraitMask = std::uint32_t;

inline constexpr TraitMask kStatelessOneToOne = 1u << 0;  ///< pure map (scale/abs) — lazy read-path allowed
inline constexpr TraitMask kCausalStateful = 1u << 1;     ///< integral/derivative/window — EAGER required
inline constexpr TraitMask kFiniteLookback = 1u << 2;     ///< bounded look-back window
inline constexpr TraitMask kFullRecompute = 1u << 3;      ///< re-run over whole input on any change
inline constexpr TraitMask kMimoExactJoin = 1u << 4;      ///< N inputs sharing a timestamp (deferred)
inline constexpr TraitMask kMaySuppress = 1u << 5;        ///< may emit nullopt (atomic all-output suppression)
inline constexpr TraitMask kPreservesOrder = 1u << 6;     ///< output timestamps strictly ascending

/// Host-internal polymorphic base for a data processor (filter or transform).
/// Derived classes carry a C++, Lua, or Python implementation; this type NEVER
/// crosses the plugin ABI (plugins ship recipes/scripts, not subclasses). v1 is
/// SISO (`numInputs`/`numOutputs` == 1; filters first, MIMO deferred) but the
/// N->M shape is kept so MIMO is a later addition, not a redesign.
///
/// SEQUENTIAL CONTRACT (matches `pj_datastore::ISISOTransform`):
/// `calculateNextPoint` is called once per sample in strictly ascending
/// timestamp order; state persists across calls and is cleared ONLY by `reset()`
/// (the engine never resets between commits — only before a batch recompute).
class DataProcessor {
 public:
  virtual ~DataProcessor() = default;

  // ---- identity / shape ----
  [[nodiscard]] virtual const char* id() const = 0;            ///< catalog key (recipe + legend)
  [[nodiscard]] virtual const char* bracketLabel() const = 0;  ///< legend suffix, e.g. "Integral"
  [[nodiscard]] virtual TraitMask traits() const = 0;
  [[nodiscard]] virtual bool isStreamSafe() const = 0;  ///< false => recompute-from-start, never lazy
  [[nodiscard]] virtual int numInputs() const {
    return 1;
  }
  [[nodiscard]] virtual int numOutputs() const {
    return 1;
  }

  // ---- lifecycle ----
  virtual void reset() = 0;

  // ---- processing ----
  /// Per-sample step. Returns nullopt to suppress the row atomically (no output
  /// at this timestamp — e.g. the first sample of a derivative/integral).
  [[nodiscard]] virtual std::optional<Sample> calculateNextPoint(const Sample& in) = 0;

  /// Fold only the newly arrived tail, APPENDING results to `out`. Default loops
  /// `calculateNextPoint`; override for O(delta). This is what the eager engine
  /// calls on each commit.
  virtual void appendTail(const std::vector<Sample>& tail, std::vector<Sample>& out);

  /// One-shot over a whole series (preview / static reprocess). Default =
  /// `reset()` then `appendTail`.
  [[nodiscard]] virtual std::vector<Sample> applyBatch(const std::vector<Sample>& in);

  // ---- failure (sticky) ----
  /// True once the processor has hit an unrecoverable error (e.g. a script runtime
  /// error) and can no longer produce valid output. The engine MUST surface this
  /// rather than treat a failed sample as a silently-suppressed row. Default: never
  /// fails (the C++ base does not error).
  [[nodiscard]] virtual bool failed() const {
    return false;
  }
  /// Human-readable reason for `failed()`, or empty.
  [[nodiscard]] virtual const std::string& error() const {
    static const std::string kNone;
    return kNone;
  }

  // ---- typed output kinds (reuses the engine's ISISOTransform::outputKind seam) ----
  /// Declared output `StorageKind`(s) given the input kind(s). Default: kFloat64
  /// per output. Override to preserve integer types or emit bool/string.
  [[nodiscard]] virtual std::vector<PJ::StorageKind> outputKinds(PJ::Span<const PJ::StorageKind> in) const;

  // ---- params ----
  [[nodiscard]] virtual std::string saveParams() const {
    return "{}";
  }
  virtual void loadParams(const std::string& json) {
    (void)json;
  }
};

}  // namespace PJ::proc
