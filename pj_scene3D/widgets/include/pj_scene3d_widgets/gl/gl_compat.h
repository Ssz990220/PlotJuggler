#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// GL token compatibility for Apple's frozen OpenGL headers.
//
// Apple caps desktop OpenGL at 4.1 Core and its <OpenGL/gl3.h> / <OpenGL/glext.h>
// stop at GL_VERSION_4_1, so tokens introduced by later versions are simply
// undeclared there. On every other desktop platform Qt pulls the Khronos glext.h,
// which defines these regardless of the runtime driver, so the module compiles.
//
// The module references a handful of >4.1 tokens on two OPTIONAL, runtime-gated
// paths:
//   * the point-cloud AABB compute reduction (GL 4.3 compute shaders + SSBOs),
//     dispatched only when hasComputeSupport() is true (never on Apple GL 4.1),
//     with the CPU bounds scan as the fallback; and
//   * KHR_debug output (GL 4.3), installed only when the context actually exposes
//     it (see gl::installDebugCallback, guarded on GL >= 4.3 || GL_KHR_debug).
//
// Neither path executes on Apple's 4.1 context, but both must still COMPILE there.
// Define the missing token VALUES (from the Khronos OpenGL registry) so the code
// builds on macOS; they are inert unless the corresponding capability is present.
// Values must match the registry exactly — they are the same numeric constants the
// Khronos glext.h supplies on Linux/Windows.

#include <QOpenGLFunctions>  // pulls qopengl.h -> GL types and the platform GL tokens

#if defined(__APPLE__)

// --- GL 4.3 compute shaders / shader-storage buffers (ARB_compute_shader,
// --- ARB_shader_storage_buffer_object) ---------------------------------------
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

// --- GL 4.3 debug output (KHR_debug) -----------------------------------------
#ifndef GL_DEBUG_OUTPUT
#define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
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
