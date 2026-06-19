#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <glm/glm.hpp>
#include <string>
#include <vector>

#include "pj_scene3d_core/tf/transform.h"  // TimePoint

namespace pj::scene3d {

class TransformBuffer;

// One drawn TF parent-connection edge: the world-space origins (expressed in the
// fixed frame) of a child frame and its parent. Endpoints are float — GL-ready
// and matching the float downcast the rest of the 3D pipeline applies to the
// double-precision TransformBuffer poses.
struct TfConnectionSegment {
  glm::vec3 child;
  glm::vec3 parent;
};

// Append one segment per NON-root frame in `tf`: a line from the frame's origin
// to its parent's origin, both resolved into `fixed_frame` at `time`. A frame is
// skipped when it has no parent (a forest root) or when EITHER endpoint fails to
// resolve at `time` (no sample yet, disconnected, …) — a half-resolved edge is
// never emitted. `out` is cleared first, then reused, so a per-paint caller
// avoids reallocating. Frame order follows TransformBuffer::getAllFrames.
void buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time, std::vector<TfConnectionSegment>& out);

// Allocating convenience overload. Prefer the out-parameter version on the
// render hot path.
[[nodiscard]] std::vector<TfConnectionSegment> buildTfConnectionSegments(
    const TransformBuffer& tf, const std::string& fixed_frame, TimePoint time);

}  // namespace pj::scene3d
