// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/render_pass.h"

#include "pj_scene3d_core/tf/tf_buffer.h"

namespace pj::scene3d {

std::optional<Transform> FrameContext::lookup(const std::string& child) const {
  auto result = tf.tryLookupTransform(fixed_frame, child, time);
  if (!result) {
    return std::nullopt;
  }
  // World → render space: shift the translation by the camera-relative origin in
  // double precision (rotation is unaffected). With render_origin == {0,0,0} this is
  // a no-op and the transform stays in absolute fixed-frame coordinates.
  Transform render_space = *result;
  render_space.t -= render_origin;
  return render_space;
}

}  // namespace pj::scene3d
