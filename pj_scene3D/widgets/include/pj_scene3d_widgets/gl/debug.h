#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace pj::scene3d::gl {

// True when GL debug output should be enabled for this process.
// Returns true in debug builds (#ifndef NDEBUG) or when PJ_GL_DEBUG=1 in the
// environment, so release builds can opt in without recompiling.
// Used by make_default_format() to request a DebugContext surface and by
// installDebugCallback() to decide whether to install the hook at all.
bool debugOutputRequested();

// Install a glDebugMessageCallback (KHR_debug / GL 4.3+) that forwards driver
// diagnostics to stderr. Requires a current GL context; no-op when:
//   - debugOutputRequested() is false (release build, PJ_GL_DEBUG unset), or
//   - the extension is absent (GL < 4.3 and no GL_KHR_debug), or
//   - no current context.
// GL_DEBUG_OUTPUT_SYNCHRONOUS is enabled so messages are delivered inline on
// the calling thread — only useful in an explicit debugging session, which is
// the only time this function does anything. Must be called inside initializeGL
// or paintGL (i.e. with the context current).
void installDebugCallback();

}  // namespace pj::scene3d::gl
