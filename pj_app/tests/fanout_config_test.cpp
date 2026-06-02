// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QString>
#include <string>
#include <vector>

#include "FanoutConfig.h"

namespace {

using PJ::detail::extractFanout;
using PJ::detail::parseDisplayName;
using PJ::detail::parseDisplaySuffix;
using Vec = std::vector<std::string>;

// --- extractFanout: fallback-to-single-instance paths -----------------------

TEST(ExtractFanout, EmptyConfigFallsBackToSingleInstance) {
  EXPECT_EQ(extractFanout(""), Vec{""});
}

TEST(ExtractFanout, NonObjectJsonReturnsConfigVerbatim) {
  EXPECT_EQ(extractFanout("not json"), Vec{"not json"});
  EXPECT_EQ(extractFanout("[1,2]"), Vec{"[1,2]"});
}

TEST(ExtractFanout, MissingKeyRunsSingleInstance) {
  const std::string cfg = R"({"foo":1})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

TEST(ExtractFanout, NonArrayValueFallsBack) {
  const std::string cfg = R"({"__pj_fanout":"x"})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

TEST(ExtractFanout, EmptyArrayFallsBack) {
  const std::string cfg = R"({"__pj_fanout":[]})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

// --- extractFanout: fanout paths -------------------------------------------

TEST(ExtractFanout, HappyPathReturnsEntriesInOrder) {
  const std::string cfg = R"({"__pj_fanout":["a","b"]})";
  EXPECT_EQ(extractFanout(cfg), (Vec{"a", "b"}));
}

// A malformed (non-string) entry must be skipped, not silently collapse the
// whole array — the survivors are still fanned out.
TEST(ExtractFanout, NonStringEntriesAreSkipped) {
  const std::string cfg = R"({"__pj_fanout":["a",5,"b"]})";
  EXPECT_EQ(extractFanout(cfg), (Vec{"a", "b"}));
}

// If skipping leaves nothing usable, fall back to single-instance — NOT an
// empty vector (an empty vector would send loadFile into the fanout branch with
// zero iterations, importing nothing while reporting success).
TEST(ExtractFanout, AllNonStringEntriesFallBackToSingleInstance) {
  const std::string cfg = R"({"__pj_fanout":[1,2]})";
  EXPECT_EQ(extractFanout(cfg), Vec{cfg});
}

// --- parseDisplaySuffix -----------------------------------------------------

TEST(ParseDisplaySuffix, EmptyConfigReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix("", QStringLiteral("fb")).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, NonObjectReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix("not json", QStringLiteral("fb")).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, MissingKeyReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"foo":"bar"})", QStringLiteral("fb")).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, NonStringValueReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"display_suffix":7})", QStringLiteral("fb")).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, EmptyStringValueReturnsFallback) {
  EXPECT_EQ(parseDisplaySuffix(R"({"display_suffix":""})", QStringLiteral("fb")).toStdString(), std::string("fb"));
}

TEST(ParseDisplaySuffix, PresentValueReturnsSuffix) {
  EXPECT_EQ(
      parseDisplaySuffix(R"({"display_suffix":"cam_left"})", QStringLiteral("fb")).toStdString(),
      std::string("cam_left"));
}

// --- parseDisplayName -------------------------------------------------------

TEST(ParseDisplayName, EmptyConfigReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName("").isEmpty());
}

TEST(ParseDisplayName, NonObjectReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName("not json").isEmpty());
  EXPECT_TRUE(parseDisplayName("[1,2]").isEmpty());
}

TEST(ParseDisplayName, MissingKeyReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"foo":"bar"})").isEmpty());
}

TEST(ParseDisplayName, NonStringValueReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"display_name":7})").isEmpty());
}

TEST(ParseDisplayName, EmptyStringValueReturnsEmpty) {
  EXPECT_TRUE(parseDisplayName(R"({"display_name":""})").isEmpty());
}

TEST(ParseDisplayName, PresentValueReturnsName) {
  EXPECT_EQ(parseDisplayName(R"({"display_name":"pusht_v21"})").toStdString(), std::string("pusht_v21"));
}

}  // namespace
