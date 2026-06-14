// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/gpu_profiler.h"

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {

GpuProfiler::~GpuProfiler() {
  releaseGL();
}

void GpuProfiler::beginFrame() {
  if (QOpenGLContext::currentContext() == nullptr) {
    return;
  }
  // GL_TIME_ELAPSED and its 64-bit result are desktop-GL only (no
  // QOpenGLExtraFunctions / ES fallback), so resolve the 4.5 core set directly.
  withCoreGlFunctions([this](auto& functions) {
    unsigned int& query = queries_[static_cast<std::size_t>(write_)];
    if (query == 0U) {
      functions.glGenQueries(1, &query);
    } else if (pending_[static_cast<std::size_t>(write_)]) {
      // This slot's query was issued kRingDepth frames ago — its result is ready
      // (check anyway so we never block). Feed it into the moving average.
      GLuint available = GL_FALSE;
      functions.glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
      if (available == GL_TRUE) {
        GLuint64 elapsed_ns = 0;
        functions.glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed_ns);
        avg_.add(static_cast<double>(elapsed_ns) / 1.0e6);
      }
      pending_[static_cast<std::size_t>(write_)] = false;
    }
    functions.glBeginQuery(GL_TIME_ELAPSED, query);
  });
  active_ = true;
}

void GpuProfiler::endFrame() {
  if (!active_) {
    return;
  }
  active_ = false;
  withCoreGlFunctions([](auto& functions) { functions.glEndQuery(GL_TIME_ELAPSED); });
  pending_[static_cast<std::size_t>(write_)] = true;
  write_ = (write_ + 1) % kRingDepth;
}

void GpuProfiler::releaseGL() {
  withGlFunctionsNoThrow([this](auto& functions) {
    for (unsigned int& query : queries_) {
      if (query != 0U) {
        functions.glDeleteQueries(1, &query);
        query = 0U;
      }
    }
  });
  queries_.fill(0U);
  pending_.fill(false);
  write_ = 0;
  active_ = false;
  avg_ = MovingAverage{};
}

}  // namespace pj::scene3d::gl
