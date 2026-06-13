// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_widgets/gl/debug.h"

#include <fmt/core.h>

#include <QOpenGLContext>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <cstdio>
#include <string_view>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d::gl {
namespace {

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

// QOPENGLF_APIENTRY expands to GLAPIENTRY / __stdcall / empty as appropriate for
// the platform — required so the driver's callback ABI matches the function pointer
// type passed to glDebugMessageCallback.
void QOPENGLF_APIENTRY debugMessageCallback(
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

bool debugOutputRequested() {
#ifndef NDEBUG
  return true;
#else
  return qEnvironmentVariableIntValue("PJ_GL_DEBUG") != 0;
#endif
}

void installDebugCallback() {
  if (!debugOutputRequested()) {
    return;
  }

  // Guard availability: KHR_debug is core in GL 4.3+; check extension on older
  // contexts so we never call glDebugMessageCallback on a context that doesn't
  // have it.
  QOpenGLContext* ctx = QOpenGLContext::currentContext();
  if (ctx == nullptr) {
    return;
  }
  const QSurfaceFormat fmt = ctx->format();
  const bool has_debug = (fmt.version() >= qMakePair(4, 3)) || ctx->hasExtension(QByteArrayLiteral("GL_KHR_debug"));
  if (!has_debug) {
    return;
  }

  withGlFunctions([](auto& functions) {
    functions.glEnable(GL_DEBUG_OUTPUT);
    // GL_DEBUG_OUTPUT_SYNCHRONOUS is only useful in an explicit debugging session
    // (it serializes all GL calls to the CPU thread). We only reach here when the
    // user opted in (debug build or PJ_GL_DEBUG=1), so the cost is acceptable.
    functions.glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    functions.glDebugMessageCallback(&debugMessageCallback, nullptr);
    functions.glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
  });
}

}  // namespace pj::scene3d::gl
