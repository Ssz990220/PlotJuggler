#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFunctions_4_5_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPointer>
#include <stdexcept>

// -----------------------------------------------------------------------------
// EXPERIMENTAL macOS launch-test shim — NOT a real port.
//
// Apple's OpenGL is frozen at 4.1 Core; its <OpenGL/gl3.h> headers omit every
// enum added in GL 4.3 (compute shaders, SSBOs, KHR_debug). On Linux Qt pulls
// the Khronos glext.h, which defines these regardless of driver, so the code
// compiles there. On macOS the enums are simply undeclared and the 3D widget
// module fails to compile.
//
// This block defines only the missing enum *values* so the module compiles and
// pj_app can be linked/launched for triage. It provides NO runtime capability:
// the compute / SSBO passes and KHR_debug will not function on Apple's 4.1
// context (withCoreGlFunctions already throws when the 4.5 core set does not
// resolve, which is always the case here). Do not treat this as macOS 3D
// support — the point-cloud compute-reduction path must be ported to a
// compute-free fallback (or Metal/Vulkan) before 3D works on macOS.
#if defined(__APPLE__)
#ifndef GL_DEBUG_OUTPUT
#define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
#define GL_SHADER_STORAGE_BARRIER_BIT 0x00002000
#endif
#ifndef GL_BUFFER_UPDATE_BARRIER_BIT
#define GL_BUFFER_UPDATE_BARRIER_BIT 0x00000200
#endif
#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#endif
#ifndef GL_DEBUG_SEVERITY_LOW
#define GL_DEBUG_SEVERITY_LOW 0x9148
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#endif
#ifndef GL_DEBUG_SOURCE_API
#define GL_DEBUG_SOURCE_API 0x8246
#endif
#ifndef GL_DEBUG_SOURCE_WINDOW_SYSTEM
#define GL_DEBUG_SOURCE_WINDOW_SYSTEM 0x8247
#endif
#ifndef GL_DEBUG_SOURCE_SHADER_COMPILER
#define GL_DEBUG_SOURCE_SHADER_COMPILER 0x8248
#endif
#ifndef GL_DEBUG_SOURCE_THIRD_PARTY
#define GL_DEBUG_SOURCE_THIRD_PARTY 0x8249
#endif
#ifndef GL_DEBUG_SOURCE_APPLICATION
#define GL_DEBUG_SOURCE_APPLICATION 0x824A
#endif
#ifndef GL_DEBUG_SOURCE_OTHER
#define GL_DEBUG_SOURCE_OTHER 0x824B
#endif
#ifndef GL_DEBUG_TYPE_ERROR
#define GL_DEBUG_TYPE_ERROR 0x824C
#endif
#ifndef GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR
#define GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR 0x824D
#endif
#ifndef GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR
#define GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR 0x824E
#endif
#ifndef GL_DEBUG_TYPE_PORTABILITY
#define GL_DEBUG_TYPE_PORTABILITY 0x824F
#endif
#ifndef GL_DEBUG_TYPE_PERFORMANCE
#define GL_DEBUG_TYPE_PERFORMANCE 0x8250
#endif
#ifndef GL_DEBUG_TYPE_OTHER
#define GL_DEBUG_TYPE_OTHER 0x8251
#endif
#ifndef GL_DEBUG_TYPE_MARKER
#define GL_DEBUG_TYPE_MARKER 0x8268
#endif
#ifndef GL_DEBUG_TYPE_PUSH_GROUP
#define GL_DEBUG_TYPE_PUSH_GROUP 0x8269
#endif
#ifndef GL_DEBUG_TYPE_POP_GROUP
#define GL_DEBUG_TYPE_POP_GROUP 0x826A
#endif
#endif  // __APPLE__

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
