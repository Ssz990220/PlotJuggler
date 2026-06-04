#include <gtest/gtest.h>

#include <QString>

#include "pj_runtime/CurveColorRegistry.h"

namespace {

TEST(CurveColorRegistryTest, UnknownCurveHasNoRememberedColor) {
  PJ::CurveColorRegistry registry;
  EXPECT_FALSE(registry.color(QStringLiteral("/imu/accel/x")).has_value());
}

TEST(CurveColorRegistryTest, RemembersColorPerCurveKey) {
  PJ::CurveColorRegistry registry;
  registry.setColor(QStringLiteral("/imu/accel/x"), QStringLiteral("#1f77b4"));

  const auto color = registry.color(QStringLiteral("/imu/accel/x"));
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(*color, QStringLiteral("#1f77b4"));
  // A different key is still unknown.
  EXPECT_FALSE(registry.color(QStringLiteral("/imu/accel/y")).has_value());
}

TEST(CurveColorRegistryTest, SetColorOverwritesPreviousAssignment) {
  PJ::CurveColorRegistry registry;
  registry.setColor(QStringLiteral("/speed"), QStringLiteral("#1f77b4"));
  registry.setColor(QStringLiteral("/speed"), QStringLiteral("#d62728"));

  const auto color = registry.color(QStringLiteral("/speed"));
  ASSERT_TRUE(color.has_value());
  EXPECT_EQ(*color, QStringLiteral("#d62728"));
}

TEST(CurveColorRegistryTest, NextPaletteIndexIncrementsMonotonically) {
  PJ::CurveColorRegistry registry;
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
  EXPECT_EQ(registry.nextPaletteIndex(), 1);
  EXPECT_EQ(registry.nextPaletteIndex(), 2);
}

TEST(CurveColorRegistryTest, ClearForgetsColorsAndResetsPaletteIndex) {
  PJ::CurveColorRegistry registry;
  registry.setColor(QStringLiteral("/speed"), QStringLiteral("#1f77b4"));
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
  EXPECT_EQ(registry.nextPaletteIndex(), 1);

  registry.clear();

  EXPECT_FALSE(registry.color(QStringLiteral("/speed")).has_value());
  EXPECT_EQ(registry.nextPaletteIndex(), 0);
}

}  // namespace
