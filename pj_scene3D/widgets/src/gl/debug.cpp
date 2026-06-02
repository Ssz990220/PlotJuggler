// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/debug.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace pj::scene3d::gl {
namespace {

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

std::string_view sourceLabel(GLenum source) {
  switch (source) {
    case GL_DEBUG_SOURCE_API:
      return "api";
    case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
      return "window-system";
    case GL_DEBUG_SOURCE_SHADER_COMPILER:
      return "shader-compiler";
    case GL_DEBUG_SOURCE_THIRD_PARTY:
      return "third-party";
    case GL_DEBUG_SOURCE_APPLICATION:
      return "application";
    case GL_DEBUG_SOURCE_OTHER:
      return "other";
    default:
      return "unknown";
  }
}

std::string_view typeLabel(GLenum type) {
  switch (type) {
    case GL_DEBUG_TYPE_ERROR:
      return "error";
    case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
      return "deprecated-behavior";
    case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
      return "undefined-behavior";
    case GL_DEBUG_TYPE_PORTABILITY:
      return "portability";
    case GL_DEBUG_TYPE_PERFORMANCE:
      return "performance";
    case GL_DEBUG_TYPE_MARKER:
      return "marker";
    case GL_DEBUG_TYPE_PUSH_GROUP:
      return "push-group";
    case GL_DEBUG_TYPE_POP_GROUP:
      return "pop-group";
    case GL_DEBUG_TYPE_OTHER:
      return "other";
    default:
      return "unknown";
  }
}

std::string_view severityLabel(GLenum severity) {
  switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:
      return "high";
    case GL_DEBUG_SEVERITY_MEDIUM:
      return "medium";
    case GL_DEBUG_SEVERITY_LOW:
      return "low";
    case GL_DEBUG_SEVERITY_NOTIFICATION:
      return "notification";
    default:
      return "unknown";
  }
}

void debugMessageCallback(
    GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message,
    const void* user_param) {
  static_cast<void>(user_param);
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
    return;
  }

  const std::string_view text{
      message,
      length > 0 ? static_cast<std::size_t>(length) : std::char_traits<char>::length(message),
  };
  fmt::print(
      stderr, "OpenGL {} {} from {} [{}]: {}\n", severityLabel(severity), typeLabel(type), sourceLabel(source), id,
      text);
}

}  // namespace

void installDebugCallback() {
  withGlFunctions([](auto& functions) {
    functions.glEnable(GL_DEBUG_OUTPUT);
    functions.glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    functions.glDebugMessageCallback(&debugMessageCallback, nullptr);
    functions.glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
  });
}

}  // namespace pj::scene3d::gl
