// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/sample.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace PJ::proc {
namespace {

TEST(SampleTest, ScalarHoldsTimestampAndSingleValue) {
  const Sample s = Sample::scalar(123, PJ::VarValue{4.5});
  EXPECT_EQ(s.raw_ts_ns, 123);
  ASSERT_EQ(s.channelCount(), 1u);
  ASSERT_TRUE(std::holds_alternative<double>(s.value()));
  EXPECT_DOUBLE_EQ(std::get<double>(s.value()), 4.5);
}

TEST(SampleTest, DefaultIsEmpty) {
  const Sample s;
  EXPECT_EQ(s.raw_ts_ns, 0);
  EXPECT_EQ(s.channelCount(), 0u);
}

}  // namespace
}  // namespace PJ::proc
