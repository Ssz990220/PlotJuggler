// SPDX-License-Identifier: MPL-2.0
#include "RasterKeyMap.h"

#include <Qt>

namespace PJ {

namespace {
// Engine key codes (interface facts; not copied from any GPL header).
constexpr int kRight = 0xae;
constexpr int kLeft = 0xac;
constexpr int kUp = 0xad;
constexpr int kDown = 0xaf;
constexpr int kUse = 0xa2;
constexpr int kFire = 0xa3;
constexpr int kEscape = 27;
constexpr int kEnter = 13;
constexpr int kTab = 9;
constexpr int kBackspace = 0x7f;
constexpr int kRShift = 0x80 + 0x36;
constexpr int kRCtrl = 0x80 + 0x1d;
constexpr int kRAlt = 0x80 + 0x38;
}  // namespace

int engineKeyForQtKey(int qt_key) {
  switch (qt_key) {
    case Qt::Key_Right:
      return kRight;
    case Qt::Key_Left:
      return kLeft;
    case Qt::Key_Up:
      return kUp;
    case Qt::Key_Down:
      return kDown;
    case Qt::Key_Space:
      return kUse;
    case Qt::Key_Control:
      return kFire;
    case Qt::Key_Escape:
      return kEscape;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      return kEnter;
    case Qt::Key_Tab:
      return kTab;
    case Qt::Key_Backspace:
      return kBackspace;
    case Qt::Key_Shift:
      return kRShift;
    case Qt::Key_Meta:
      return kRCtrl;
    case Qt::Key_Alt:
      return kRAlt;
    default:
      break;
  }
  if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
    return (qt_key - Qt::Key_A) + 'a';
  }
  if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9) {
    return (qt_key - Qt::Key_0) + '0';
  }
  return 0;
}

}  // namespace PJ
