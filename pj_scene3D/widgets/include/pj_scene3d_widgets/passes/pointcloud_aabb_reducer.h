#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLFunctions_4_5_Core>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "pj_scene3d_core/camera/camera.h"  // AABB

namespace pj::scene3d {

namespace gl {
class Program;
}

// GPU compute reduction of a point cloud's geometric AABB, replacing the CPU
// bounds scan on the hot streaming path.
//
// Why this exists: the zero-copy fast path already uploads the wire cloud to a
// VBO for rendering. A compute shader reduces THAT resident VRAM buffer to its
// min/max corner with no extra upload and zero CPU scan, so the per-sample bounds
// cost leaves the GUI thread entirely. The reduction is read back ASYNCHRONOUSLY
// (fence + non-blocking poll), so dispatch never stalls the pipeline; the result
// lands a frame or two later, which is invisible to the camera auto-fit.
//
// Threading / context: every method touches GL and MUST run on the owning
// QOpenGLWidget's render thread with its context current (i.e. from a render
// pass's render()/releaseGL()). The reducer self-initialises lazily on the first
// dispatch(); available() is only meaningful after that first call.
//
// Atomic float min/max is done via the order-preserving key in
// pj_scene3d_core/aabb_gpu_key.h — the GLSL mirror lives in the .cpp.
class PointcloudAabbReducer {
 public:
  PointcloudAabbReducer();
  ~PointcloudAabbReducer();

  PointcloudAabbReducer(const PointcloudAabbReducer&) = delete;
  PointcloudAabbReducer& operator=(const PointcloudAabbReducer&) = delete;

  // Strided float32 xyz source. `source_buffer` is any GL buffer object holding
  // the points (the fast-path VBO); `stride_bytes` is the per-point step and
  // `x_offset_bytes` the byte offset of x within it (y/z are assumed contiguous
  // at +4 / +8 — exactly the fast path's contiguous-float32 xyz). All three byte
  // quantities MUST be 4-byte aligned (the shader indexes the buffer as uint
  // words); the caller guarantees this via gpuEligible().
  void dispatch(GLuint source_buffer, std::size_t point_count, uint32_t stride_bytes, uint32_t x_offset_bytes);

  // Non-blocking. Returns a freshly read-back AABB once the fence from a prior
  // dispatch() has signalled (then clears the fence), otherwise std::nullopt. A
  // dispatch over a cloud with no finite points yields an AABB with valid=false.
  [[nodiscard]] std::optional<AABB> poll();

  // True once the compute program has compiled+linked successfully (i.e. the
  // context is GL >= 4.3 with compute support). Meaningful only after the first
  // dispatch(); false before that and on any compute-unsupported context.
  [[nodiscard]] bool available() const {
    return available_;
  }

  // True once dispatch() has been attempted at least once (so available() is
  // authoritative). Lets the layer distinguish "not yet probed" from "probed,
  // unsupported".
  [[nodiscard]] bool probed() const {
    return probed_;
  }

  // Drop all GL resources under the dying context (called from the pass's
  // releaseGL()). A later dispatch() re-initialises lazily.
  void releaseGL();

  // The byte alignment the source layout must satisfy for dispatch() — exposed so
  // the layer can gate eligibility without hard-coding the constant.
  static constexpr uint32_t kRequiredAlignmentBytes = 4;

 private:
  bool ensureInitialized();
  void destroyFence();

  std::unique_ptr<gl::Program> program_;
  GLuint result_buffer_{0};
  GLsync fence_{nullptr};
  bool pending_{false};    // a dispatch fence is outstanding, awaiting poll()
  bool available_{false};  // compute program compiled+linked
  bool probed_{false};     // dispatch() attempted at least once
};

}  // namespace pj::scene3d
