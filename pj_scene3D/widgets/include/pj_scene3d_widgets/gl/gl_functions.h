#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPointer>
#include <stdexcept>

namespace pj::scene3d {

// OpenGL function-set acquisition shared by every render pass, gizmo, and gl::
// RAII wrapper so the boilerplate lives in exactly one place. Three flavors,
// each for a distinct call-site contract:
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
//   withCoreGlFunctions   — THROWS unless the 4.5 core profile resolves. Use
//                           only for 4.5-only entry points (e.g.
//                           glTexImage2DMultisample) that have no
//                           QOpenGLExtraFunctions fallback.
//
// All three call initializeOpenGLFunctions() on the resolved set before
// invoking the callback. The wrappers in gl/ are namespace pj::scene3d::gl, so
// they qualify these as pj::scene3d::withGlFunctions (or add a using).

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

// 4.5-core-only acquisition: throws unless QOpenGLFunctions_4_5_Core resolves
// (no QOpenGLExtraFunctions fallback). For desktop-GL-only entry points.
template <typename Callback>
decltype(auto) withCoreGlFunctions(Callback&& callback) {
  QOpenGLContext* context = QOpenGLContext::currentContext();
  if (context == nullptr) {
    throw std::runtime_error("No current OpenGL context");
  }
  auto* functions = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_5_Core>(context);
  if (functions == nullptr) {
    throw std::runtime_error("No OpenGL 4.5 Core functions available");
  }
  functions->initializeOpenGLFunctions();
  return callback(*functions);
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
