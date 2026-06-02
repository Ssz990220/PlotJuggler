// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace PJ {

// Maps a Qt::Key value to the engine's key code (the numeric protocol the render
// helper expects). Returns 0 for keys we don't forward.
int engineKeyForQtKey(int qt_key);

}  // namespace PJ
