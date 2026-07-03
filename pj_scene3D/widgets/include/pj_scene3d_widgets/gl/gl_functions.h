#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_1_Core>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPointer>
#include <stdexcept>

#include "pj_scene3d_widgets/gl/gl_compat.h"  // GL >4.1 tokens on Apple's frozen headers

namespace pj::scene3d {

// OpenGL function-set acquisition shared by every render pass, gizmo, and gl::
// RAII wrapper so the boilerplate lives in exactly one place. The module targets
// a GL 4.1 Core BASELINE — the highest desktop profile Apple exposes — so the
// whole render path resolves through function sets available at 4.1. Compute is
// the one optional capability above the baseline; see withComputeGlFunctions.
//
//   withGlFunctions       — THROWS if there is no current context / no usable
//                           function set. Use on render / initializeGL paths,
//                           which always run inside paintGL with the context
//                           current; a missing context there is a real bug.
//   withGlFunctionsNoThrow — silently returns when no context / functions are
//                           available. Use on TEARDOWN paths (destructors,
//                           move-assignments, releaseGL), where a context may
//                           legitimately have already been destroyed and an
//                           exception out of a destructor is forbidden.
//   withCoreGlFunctions   — THROWS unless the 4.1 core profile resolves. Use for
//                           desktop-core entry points (e.g.
//                           glTexImage2DMultisample [3.2], timer queries [3.3])
//                           that have no QOpenGLExtraFunctions (GLES) fallback but
//                           ARE part of the 4.1 baseline.
//   withComputeGlFunctions — resolves the GL 4.3 core set (compute shaders,
//                           SSBOs, glMemoryBarrier). Returns false WITHOUT calling
//                           the callback when the context is below 4.3 (Apple's
//                           4.1), so the caller can fall back — it never throws.
//
// The versioned wrappers call initializeOpenGLFunctions() on the resolved set
// before invoking the callback. The gl/ RAII types are namespace
// pj::scene3d::gl, so they qualify these as pj::scene3d::withGlFunctions.
//
// withGlFunctions/withGlFunctionsNoThrow prefer QOpenGLFunctions_4_5_Core and
// fall back to QOpenGLExtraFunctions. On Linux this lands on 4_5_Core; on Apple
// GL (no 4_5) it lands on extraFunctions, which correctly dispatches the 4.1
// desktop entry points these callbacks use. The extraFunctions fallback is NOT a
// mere backstop: Qt's versioned QOpenGLFunctions_4_1_Core / 4_3_Core classes omit
// some functions the render/teardown callbacks call — glVertexAttrib1f (constant
// vertex attribute) and the KHR_debug entry points (gl::installDebugCallback) —
// which exist only in 4_5_Core and QOpenGLExtraFunctions. So a "walk down to
// 4_1_Core" chain does NOT compile; extraFunctions is the required Apple path and
// is proven correct on the mac by the on-widget grab tests (tf_connections_gl_test
// et al. read back real rendered pixels at 4.1). Render sites that DO need a
// versioned-only entry point (glTexImage2DMultisample, timer queries,
// glPolygonMode, and the paintGL/renderScene direct calls) use the explicit 4.1
// resolution via withCoreGlFunctions / get<QOpenGLFunctions_4_1_Core> instead.

template <typename Callback>
decltype(auto) withGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }
  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    return callback(*functions);
  }
  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
}

// Teardown-safe acquisition: returns without invoking `callback` when no
// context or function set is current. noexcept so it is safe in destructors.
template <typename Callback>
void withGlFunctionsNoThrow(Callback&& callback) noexcept {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return;
  }
  if (auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context); functions != nullptr) {
    functions->initializeOpenGLFunctions();
    callback(*functions);
    return;
  }
  QOpenGLExtraFunctions* functions = context->extraFunctions();
  if (functions == nullptr) {
    return;
  }
  functions->initializeOpenGLFunctions();
  callback(*functions);
}

// 4.1-core acquisition: throws unless QOpenGLFunctions_4_1_Core resolves (no
// QOpenGLExtraFunctions fallback). For desktop-core entry points that are part of
// the 4.1 baseline yet absent from the GLES-shaped QOpenGLExtraFunctions
// (glTexImage2DMultisample, GL_TIME_ELAPSED timer queries, glPolygonMode). Any
// GL >= 4.1 core context — including Apple's frozen 4.1 and Linux's 4.5 — resolves
// this, so it is not a capability gate; a null here is a genuine bug.
template <typename Callback>
decltype(auto) withCoreGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }
  auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_1_Core>(context);
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL 4.1 Core functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
}

// True when the current context can run GL 4.3 compute shaders + SSBOs. False on
// Apple's 4.1 (and any pre-4.3 context), where the point-cloud AABB reduction
// falls back to the CPU bounds scan. Uses the function-set factory rather than the
// reported version string so it reflects what Qt can actually resolve.
inline bool hasComputeSupport() {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  return context != nullptr &&
         QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_3_Core>(context) != nullptr;
}

// Optional GL 4.3 compute acquisition. Resolves QOpenGLFunctions_4_3_Core and
// invokes `callback` with it, returning true; returns false WITHOUT calling the
// callback when there is no current context or the context is below 4.3. Never
// throws — the compute point-cloud AABB reducer is the sole caller and treats
// false as "no compute, use the CPU scan".
template <typename Callback>
bool withComputeGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    return false;
  }
  auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_3_Core>(context);
  if (functions == nullptr) {
    return false;
  }
  functions->initializeOpenGLFunctions();
  callback(*functions);
  return true;
}

namespace gl {

// True when the currently-current context may legally delete a GL name owned by
// `owning_context`. The app deliberately does NOT share contexts
// (AA_ShareOpenGLContexts is off), so every view has an independent name space:
// deleting under a foreign context either no-ops the real owner (leak) or frees
// an unrelated name that happens to collide numerically (cross-view corruption).
//
// Returns true when a context is current AND it is the owner (or shares a name
// space with it). Returns false — caller should DROP the id, not delete — when:
//   - no context is current (teardown after the owner already died), or
//   - owning_context is null/destroyed (the driver freed the name with the
//     context, so the id is already gone), or
//   - a *different*, non-sharing context is current.
inline bool deleteAllowedInCurrentContext(const QPointer<QOpenGLContext>& owning_context) {
  QOpenGLContext* current = QOpenGLContext::currentContext();
  if (current == nullptr || owning_context.isNull()) {
    return false;
  }
  return current == owning_context || QOpenGLContext::areSharing(current, owning_context);
}

}  // namespace gl

// Unbind the current shader program (glUseProgram(0)). Shared by passes that
// restore default program state after drawing.
inline void unuseProgram() {
  withGlFunctions([](auto& functions) { functions.glUseProgram(0U); });
}

}  // namespace pj::scene3d
