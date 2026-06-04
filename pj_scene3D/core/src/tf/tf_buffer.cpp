// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/tf/tf_buffer.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pj::scene3d {

// EdgeHistory and ParentLink are defined in tf_buffer.h (required complete by
// std::unordered_map on GCC 11's libstdc++).

TransformBuffer::TransformBuffer(Duration cache_window) : cache_window_(cache_window) {}

TransformBuffer::~TransformBuffer() = default;

PJ::Expected<void, SetTransformError> TransformBuffer::setTransform(const StampedTransform& tf) {
  std::unique_lock lock(parents_mutex_);

  if (tf.child_frame == tf.parent_frame) {
    // A frame relative to itself is the identity; storing it would create a
    // self-loop in the parent map and hang chainToRoot. Drop it (recoverable).
    return PJ::unexpected(SetTransformError::SelfLoop);
  }

  auto& link = parents_[tf.child_frame];
  if (!link.parent.empty() && link.parent != tf.parent_frame) {
    // A second publisher claims this child under a different parent. Drop this
    // one edge rather than aborting the whole bulk ingest.
    return PJ::unexpected(SetTransformError::ReparentConflict);
  }

  link.parent = tf.parent_frame;

  // No static/dynamic distinction: every edge is a time-ordered history resolved
  // by nearest-previous (see sampleAt). A transform published once — /tf_static,
  // however namespaced — is simply a single-sample history that holds for all
  // later query times, so static-ness never has to be inferred from a topic name.
  const Transform normalized{tf.transform.t, glm::normalize(tf.transform.q)};
  auto& samples = link.history.samples;
  if (samples.empty() || tf.stamp > samples.back().first) {
    samples.emplace_back(tf.stamp, normalized);
  } else {
    auto it = std::lower_bound(samples.begin(), samples.end(), tf.stamp, EdgeHistory::less_stamp);
    if (it != samples.end() && it->first == tf.stamp) {
      it->second = normalized;
    } else {
      samples.insert(it, {tf.stamp, normalized});
    }
  }

  // kKeepAll disables eviction: bulk-ingested bounded sources (a loaded file)
  // feed the whole recording up front, so a rolling window would trim every
  // edge to its tail and break lookups earlier in the timeline. Under a finite
  // window (live streaming) `samples.size() > 1` pins the most recent sample —
  // an edge is NEVER evicted to empty, so a slow / stopped / once-published frame
  // keeps resolving forever instead of orphaning. That "always keep the last
  // sample" guarantee is what lets us drop the static flag: it gives static-like
  // persistence to every edge uniformly. (The current cutoff is per-edge-relative
  // so the guard is presently belt-and-suspenders, but it pins the invariant
  // should eviction ever switch to a global clock.)
  if (cache_window_ < kKeepAll) {
    const auto cutoff = samples.back().first - cache_window_;
    while (samples.size() > 1 && samples.front().first < cutoff) {
      samples.pop_front();
    }
  }
  return {};
}

std::optional<Transform> TransformBuffer::sampleAt(const EdgeHistory& h, TimePoint t) {
  if (h.samples.empty()) {
    return std::nullopt;
  }
  // Nearest-previous (zero-order hold): the newest sample at or before `t`. A
  // single-sample edge therefore resolves at every t >= its stamp. A query
  // strictly before the first sample has no value yet (the frame had not been
  // announced at that time) -> nullopt.
  auto hi = std::upper_bound(h.samples.begin(), h.samples.end(), t, EdgeHistory::stamp_less);
  if (hi == h.samples.begin()) {
    return std::nullopt;
  }
  return std::prev(hi)->second;
}

std::vector<TransformBuffer::ChainHop> TransformBuffer::chainToRoot(const std::string& frame) const {
  std::vector<ChainHop> chain;
  chain.push_back(ChainHop{frame, nullptr});
  for (std::string cur = frame;;) {
    auto it = parents_.find(cur);
    if (it == parents_.end()) {
      break;  // `cur` is a root (or unknown): no outgoing edge.
    }
    // Record the resolved edge on the hop we just added, then advance to the
    // parent — so lookup composition reuses it instead of re-hashing `cur`.
    chain.back().link = &it->second;
    cur = it->second.parent;
    // Cycle guard: a repeated frame means a malformed tree (e.g. A→B→A, which
    // the reparent guard does not catch). Stop so the lookup fails cleanly
    // instead of spinning. Linear scan — chains are short, so this beats a
    // per-lookup hash set.
    const bool seen = std::any_of(chain.begin(), chain.end(), [&cur](const ChainHop& h) { return h.frame == cur; });
    if (seen) {
      break;
    }
    chain.push_back(ChainHop{cur, nullptr});
  }
  return chain;
}

TransformBuffer::MeetPoint TransformBuffer::findCommonAncestor(
    const std::vector<ChainHop>& src, const std::vector<ChainHop>& tgt) {
  // Lowest common ancestor: the first src frame (walking up from source) that
  // also appears in tgt. Linear scan — chains are short, so this is cheaper than
  // building a hash index on every lookup.
  for (std::size_t i = 0; i < src.size(); ++i) {
    for (std::size_t j = 0; j < tgt.size(); ++j) {
      if (src[i].frame == tgt[j].frame) {
        return MeetPoint{i, j, true};
      }
    }
  }
  return MeetPoint{};
}

bool TransformBuffer::isKnownFrame(const std::string& frame) const {
  if (parents_.find(frame) != parents_.end()) {
    return true;  // appears as a child
  }
  for (const auto& [child, link] : parents_) {
    if (link.parent == frame) {
      return true;  // appears as a parent (a root frame)
    }
  }
  return false;
}

PJ::Expected<Transform, LookupError> TransformBuffer::lookupTransformImpl(
    const std::string& target, const std::string& source, TimePoint stamp) const {
  if (target == source) {
    return Transform::identity();
  }

  const auto tgt_chain = chainToRoot(target);
  const auto src_chain = chainToRoot(source);

  const MeetPoint meet = findCommonAncestor(src_chain, tgt_chain);
  if (!meet.found) {
    // Distinguish "frame not in the buffer" from "known but disconnected". The
    // O(n) scans run only on this (rare) failure path, never in the steady loop.
    if (!isKnownFrame(source)) {
      return PJ::unexpected(LookupError::UnknownSource);
    }
    if (!isKnownFrame(target)) {
      return PJ::unexpected(LookupError::UnknownTarget);
    }
    return PJ::unexpected(LookupError::Disconnected);
  }

  // Compose source->common, then common->target inverse. Every hop below the
  // common ancestor carries a non-null edge (only the root hop is null).
  Transform t_common_from_source = Transform::identity();
  for (std::size_t i = 0; i < meet.src_k; ++i) {
    const auto sample = sampleAt(src_chain[i].link->history, stamp);
    if (!sample) {
      return PJ::unexpected(LookupError::NoSampleAtTime);
    }
    t_common_from_source = (*sample) * t_common_from_source;
  }

  Transform t_common_from_target = Transform::identity();
  for (std::size_t i = 0; i < meet.tgt_k; ++i) {
    const auto sample = sampleAt(tgt_chain[i].link->history, stamp);
    if (!sample) {
      return PJ::unexpected(LookupError::NoSampleAtTime);
    }
    t_common_from_target = (*sample) * t_common_from_target;
  }

  return t_common_from_target.inverse() * t_common_from_source;
}

Transform TransformBuffer::lookupTransform(
    const std::string& target, const std::string& source, TimePoint stamp) const {
  // Thin wrapper: delegate to the non-throwing accessor (which takes the lock)
  // and surface the LookupError as a message for tf2-style callers.
  auto result = tryLookupTransform(target, source, stamp);
  if (!result) {
    throw std::runtime_error("pj_scene3d: no transform from '" + source + "' to '" + target + "' at requested time");
  }
  return *result;
}

PJ::Expected<Transform, LookupError> TransformBuffer::tryLookupTransform(
    const std::string& target, const std::string& source, TimePoint stamp) const {
  std::shared_lock lock(parents_mutex_);
  return lookupTransformImpl(target, source, stamp);
}

bool TransformBuffer::canTransform(const std::string& target, const std::string& source, TimePoint stamp) const {
  return tryLookupTransform(target, source, stamp).has_value();
}

std::optional<TimePoint> TransformBuffer::latestCommonTime(const std::string& target, const std::string& source) const {
  std::shared_lock lock(parents_mutex_);

  const auto tgt_chain = chainToRoot(target);
  const auto src_chain = chainToRoot(source);

  const MeetPoint meet = findCommonAncestor(src_chain, tgt_chain);
  if (!meet.found) {
    return std::nullopt;
  }

  // Newest time bounded by every multi-sample edge up to the common ancestor.
  // Single-sample edges (the static case) and empty histories hold for all time,
  // so they impose no upper bound.
  auto walk = [](const std::vector<ChainHop>& chain, std::size_t k, std::optional<TimePoint>& acc) {
    for (std::size_t i = 0; i < k; ++i) {
      const auto& edge = chain[i].link->history;
      if (edge.samples.size() <= 1) {
        continue;
      }
      const auto latest = edge.samples.back().first;
      acc = acc ? std::min(*acc, latest) : latest;
    }
  };

  std::optional<TimePoint> result;
  walk(src_chain, meet.src_k, result);
  walk(tgt_chain, meet.tgt_k, result);
  return result.value_or(TimePoint{});
}

std::vector<std::string> TransformBuffer::getAllFrames() const {
  std::shared_lock lock(parents_mutex_);

  std::unordered_set<std::string> frames;
  frames.reserve(parents_.size() * 2U);
  for (const auto& [child, link] : parents_) {
    frames.insert(child);
    frames.insert(link.parent);
  }

  return {frames.begin(), frames.end()};
}

std::optional<std::string> TransformBuffer::getParent(const std::string& child) const {
  std::shared_lock lock(parents_mutex_);

  auto it = parents_.find(child);
  if (it == parents_.end()) {
    return std::nullopt;
  }
  return it->second.parent;
}

std::optional<TimePoint> TransformBuffer::getLatestSample(const std::string& child) const {
  std::shared_lock lock(parents_mutex_);

  auto it = parents_.find(child);
  if (it == parents_.end()) {
    return std::nullopt;
  }

  const auto& history = it->second.history;
  if (history.samples.empty()) {
    return std::nullopt;
  }
  return history.samples.back().first;
}

void TransformBuffer::clear() {
  std::unique_lock lock(parents_mutex_);
  parents_.clear();
}

namespace {

// Case-insensitive less for frame names. Frame identity stays case-sensitive
// in the TF buffer (parents_ uses unordered_map<string,...>); this comparator
// is for *display order* only so e.g. "Omniwheel_1" interleaves with
// "imu_link" instead of clustering before all lowercase frames.
struct FrameNameLess {
  bool operator()(const std::string& a, const std::string& b) const noexcept {
    return frameNameLess(a, b);
  }
};

// parent -> case-insensitively-sorted children. Built once per public-method
// call so callers see a consistent snapshot under one shared_lock. Outer map
// key (parent name) is identity-only, never compared for order.
using ChildrenMap = std::unordered_map<std::string, std::set<std::string, FrameNameLess>>;
using RootSet = std::set<std::string, FrameNameLess>;

}  // namespace

std::vector<FrameRow> TransformBuffer::getFrameHierarchy() const {
  std::shared_lock lock(parents_mutex_);

  ChildrenMap children;
  std::unordered_set<std::string> all_frames;
  std::unordered_set<std::string> non_root;
  for (const auto& [child, link] : parents_) {
    children[link.parent].insert(child);
    all_frames.insert(child);
    all_frames.insert(link.parent);
    non_root.insert(child);
  }

  RootSet roots;
  for (const auto& f : all_frames) {
    if (non_root.count(f) == 0) {
      roots.insert(f);
    }
  }

  std::vector<FrameRow> rows;
  rows.reserve(all_frames.size());
  std::unordered_set<std::string> visited;

  auto dfs = [&](auto& self, const std::string& name, int depth) -> void {
    if (!visited.insert(name).second) {
      return;
    }
    rows.push_back(FrameRow{name, depth});
    auto it = children.find(name);
    if (it == children.end()) {
      return;
    }
    for (const auto& child : it->second) {
      self(self, child, depth + 1);
    }
  };

  for (const auto& r : roots) {
    dfs(dfs, r, 0);
  }
  return rows;
}

nlohmann::json TransformBuffer::getFrameHierarchyJson() const {
  std::shared_lock lock(parents_mutex_);

  ChildrenMap children;
  std::unordered_set<std::string> all_frames;
  std::unordered_set<std::string> non_root;
  for (const auto& [child, link] : parents_) {
    children[link.parent].insert(child);
    all_frames.insert(child);
    all_frames.insert(link.parent);
    non_root.insert(child);
  }

  RootSet roots;
  for (const auto& f : all_frames) {
    if (non_root.count(f) == 0) {
      roots.insert(f);
    }
  }

  std::unordered_set<std::string> visited;
  auto build = [&](auto& self, const std::string& name) -> nlohmann::json {
    nlohmann::json node;
    node["name"] = name;
    node["children"] = nlohmann::json::array();
    if (!visited.insert(name).second) {
      return node;  // cycle guard — emit node with no children
    }
    auto it = children.find(name);
    if (it != children.end()) {
      for (const auto& child : it->second) {
        node["children"].push_back(self(self, child));
      }
    }
    return node;
  };

  nlohmann::json out = nlohmann::json::array();
  for (const auto& r : roots) {
    out.push_back(build(build, r));
  }
  return out;
}

bool frameNameLess(std::string_view a, std::string_view b) noexcept {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) < std::tolower(y);
  });
}

}  // namespace pj::scene3d
