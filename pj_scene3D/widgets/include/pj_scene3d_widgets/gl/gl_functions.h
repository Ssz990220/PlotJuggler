#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <stdexcept>

namespace pj::scene3d {

// Acquire the current OpenGL function set and invoke `callback` with it. Prefers
// the 4.5 core profile and falls back to QOpenGLExtraFunctions; throws if there
// is no current context or no usable function set. Shared by the render passes
// so the acquisition boilerplate lives in exactly one place.
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

// Unbind the current shader program (glUseProgram(0)). Shared by passes that
// restore default program state after drawing.
inline void unuseProgram() {
  withGlFunctions([](auto& functions) { functions.glUseProgram(0U); });
}

}  // namespace pj::scene3d
