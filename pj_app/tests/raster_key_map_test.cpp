// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <Qt>

#include "RasterKeyMap.h"

namespace {

using PJ::engineKeyForQtKey;

TEST(RasterKeyMap, MapsArrowsAndActions) {
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Up), 0xad);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Down), 0xaf);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Left), 0xac);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Right), 0xae);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Control), 0xa3);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Space), 0xa2);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Escape), 27);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Return), 13);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Shift), 0x80 + 0x36);
}

TEST(RasterKeyMap, MapsLettersToLowercaseAscii) {
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_Y), 'y');
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_N), 'n');
}

TEST(RasterKeyMap, UnmappedReturnsZero) {
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_F5), 0);
  EXPECT_EQ(engineKeyForQtKey(Qt::Key_MediaPlay), 0);
}

}  // namespace
