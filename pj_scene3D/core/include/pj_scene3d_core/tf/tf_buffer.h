#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <chrono>
#include <cstddef>
#include <deque>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pj_base/expected.hpp"
#include "pj_scene3d_core/tf/transform.h"

namespace pj::scene3d {

using Duration = std::chrono::nanoseconds;

// Why setTransform rejected a single edge. Both are recoverable *data* errors in
// a bulk feed (a real bag can carry either), not programmer errors — callers
// count/log and continue rather than aborting the whole ingest.
enum class SetTransformError {
  ReparentConflict,  // child already parented to a different frame
  SelfLoop,          // child == parent (a frame relative to itself)
};

// Why a TF lookup failed. An enum (not a string) keeps render-loop misses
// allocation-free; the throwing wrapper maps it back to a message.
enum class LookupError {
  UnknownSource,   // source frame not present in the buffer
  UnknownTarget,   // target frame not present in the buffer
  Disconnected,    // both frames known but no common ancestor (incl. broken cycles)
  NoSampleAtTime,  // an edge on the connecting path has no sample at the requested time
};

// Case-insensitive (ASCII) less for frame names. Frame identity stays
// case-sensitive in the buffer; this is *display order* only. It is the single
// definition of user-facing frame ordering: getFrameHierarchy()'s sibling sort
// and any dock-level merge sort (e.g. the fixed-frame combo's clusters) must
// agree, so both call this.
[[nodiscard]] bool frameNameLess(std::string_view a, std::string_view b) noexcept;

struct FrameRow {
  std::string name;
  int depth = 0;

  friend bool operator==(const FrameRow& a, const FrameRow& b) noexcept {
    return a.depth == b.depth && a.name == b.name;
  }
  friend bool operator!=(const FrameRow& a, const FrameRow& b) noexcept {
    return !(a == b);
  }
};

class TransformBuffer {
 public:
  // Pass as the cache window to disable eviction entirely (keep all samples).
  // Use this for bulk-ingested, bounded sources (e.g. a loaded file): the whole
  // recording's TF is fed in up front, so a rolling window would trim every
  // dynamic edge to its tail and break lookups earlier in the timeline. The
  // finite default suits live streaming, where it bounds memory on a growing buffer.
  static constexpr Duration kKeepAll = Duration::max();

  explicit TransformBuffer(Duration cache_window = std::chrono::seconds(10));
  ~TransformBuffer();

  // Insert or replace the edge for `tf.child_frame`. Returns the rejected-edge
  // reason instead of throwing, so a malformed edge in a bulk feed drops one
  // edge rather than aborting the whole load. There is no static/dynamic flag: a
  // transform published once (/tf_static, however it is namespaced) is just a
  // single-sample history that resolves at every later time via nearest-previous.
  PJ::Expected<void, SetTransformError> setTransform(const StampedTransform& tf);

  // Throwing lookup (tf2 ergonomics): the SE(3) target<-source transform at
  // `stamp`, or throws std::runtime_error if unavailable. Thin wrapper over
  // tryLookupTransform.
  Transform lookupTransform(const std::string& target, const std::string& source, TimePoint stamp) const;

  // Non-throwing lookup — the primary accessor. Returns the transform or a
  // LookupError reason; lookupTransform/canTransform are sugar over it.
  PJ::Expected<Transform, LookupError> tryLookupTransform(
      const std::string& target, const std::string& source, TimePoint stamp) const;

  // True iff tryLookupTransform would succeed at `stamp`.
  bool canTransform(const std::string& target, const std::string& source, TimePoint stamp) const;

  // Newest time at which every edge between the two frames has a sample
  // (single-sample edges hold for all time, so they impose no bound). nullopt if
  // the frames are disconnected.
  std::optional<TimePoint> latestCommonTime(const std::string& target, const std::string& source) const;

  std::vector<std::string> getAllFrames() const;
  std::optional<std::string> getParent(const std::string& child) const;
  std::optional<TimePoint> getLatestSample(const std::string& child) const;

  // Depth-annotated DFS pre-order of the TF forest. Roots and same-parent
  // siblings are alphabetically sorted; cycle-safe via visited set.
  std::vector<FrameRow> getFrameHierarchy() const;

  // Same forest as `getFrameHierarchy`, shaped as nested JSON:
  //   [ { "name": "odom", "children": [ { "name": "base_link", ... } ] }, ... ]
  // Intended for debug dumps, scripting integration, and layout-file diffs.
  nlohmann::json getFrameHierarchyJson() const;

  void clear();

 private:
  // Defined in-header (not forward-declared) because std::unordered_map requires
  // a complete mapped_type at member-declaration time on libstdc++ (GCC 11);
  // only newer libstdc++ tolerates an incomplete one. Keeping these inline keeps
  // the buffer portable across the supported toolchains.
  struct EdgeHistory {
    using Sample = std::pair<TimePoint, Transform>;

    std::deque<Sample> samples;

    static bool less_stamp(const Sample& sample, TimePoint stamp) {
      return sample.first < stamp;
    }
    static bool stamp_less(TimePoint stamp, const Sample& sample) {
      return stamp < sample.first;
    }
  };

  struct ParentLink {
    std::string parent;
    EdgeHistory history;
  };

  // One hop in a frame's chain toward its root. Carries the edge already resolved
  // by chainToRoot so lookup composition never re-hashes the same name (the
  // pointer is valid only while the caller holds parents_mutex_ and parents_ is
  // unmodified). `link` is null for the topmost (root) frame, which has no edge.
  struct ChainHop {
    std::string frame;
    const ParentLink* link;
  };

  // Index into a source/target chain where the two first meet, found by linear
  // scan (chains are short, so this beats per-lookup hash containers).
  struct MeetPoint {
    std::size_t src_k = 0;
    std::size_t tgt_k = 0;
    bool found = false;
  };

  std::unordered_map<std::string, ParentLink> parents_;
  mutable std::shared_mutex parents_mutex_;
  Duration cache_window_;

  static std::optional<Transform> sampleAt(const EdgeHistory& h, TimePoint t);
  // Walk `frame` to its root, recording each hop's resolved edge. Cycle-safe: a
  // repeated frame stops the walk so a malformed tree fails cleanly.
  std::vector<ChainHop> chainToRoot(const std::string& frame) const;
  // Lowest common ancestor of two chains (shared by lookup + latestCommonTime).
  static MeetPoint findCommonAncestor(const std::vector<ChainHop>& src, const std::vector<ChainHop>& tgt);
  // True if `frame` appears anywhere in the buffer (as a child or a parent).
  // Used only on the lookup-failure path to classify Unknown* vs Disconnected.
  bool isKnownFrame(const std::string& frame) const;
  PJ::Expected<Transform, LookupError> lookupTransformImpl(
      const std::string& target, const std::string& source, TimePoint stamp) const;
};

}  // namespace pj::scene3d
