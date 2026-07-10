#include <gtest/gtest.h>

#include <QString>

#include "pj_runtime/CurveColorRegistry.h"
using namespace Qt::StringLiterals;

namespace {

TEST(CurveColorRegistryTest, UnknownCurveHasNoRememberedColor) {
  PJ::CurveColorRegistry registry;
  EXPECT_FALSE(registry.color(u"/imu/accel/x"_s).has_value());
}

TEST(CurveColorRegistryTest, RemembersColorPerCurveKey) {
  PJ::CurveColorRegistry registry;
  registry.setColor(u"/imu/accel/x"_s, u"#1f77b4"_s);

  const auto color = registry.color(u"/imu/accel/x"_s);
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(*color, u"#1f77b4"_s);
  // A different key is still unknown.
  EXPECT_FALSE(registry.color(u"/imu/accel/y"_s).has_value());
}

TEST(CurveColorRegistryTest, SetColorOverwritesPreviousAssignment) {
  PJ::CurveColorRegistry registry;
  registry.setColor(u"/speed"_s, u"#1f77b4"_s);
  registry.setColor(u"/speed"_s, u"#d62728"_s);

  const auto color = registry.color(u"/speed"_s);
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(*color, u"#d62728"_s);
}

TEST(CurveColorRegistryTest, NextPaletteIndexIncrementsMonotonically) {
  PJ::CurveColorRegistry registry;
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
  EXPECT_EQ(registry.nextPaletteIndex(), 1);
  EXPECT_EQ(registry.nextPaletteIndex(), 2);
}

TEST(CurveColorRegistryTest, ClearForgetsColorsAndResetsPaletteIndex) {
  PJ::CurveColorRegistry registry;
  registry.setColor(u"/speed"_s, u"#1f77b4"_s);
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
  EXPECT_EQ(registry.nextPaletteIndex(), 1);

  registry.clear();

  EXPECT_FALSE(registry.color(u"/speed"_s).has_value());
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
}

}  // namespace
