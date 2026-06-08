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
  return *result;
}

}  // namespace pj::scene3d
