// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Smoke test for the embedded-CPython backend: proves the interpreter boots
// (PYTHONHOME / stdlib found), a filter module inspects + instantiates, and
// calculate() computes.

#include "pj_scripting/python_engine.h"

#include <gtest/gtest.h>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

namespace {
constexpr const char* kDoubleSource = R"PY(# pj-script: python
class T:
    id = "double_it"
    name = "Double"
    output = "double"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value * 2
)PY";
}  // namespace

TEST(PythonEngine, InspectReadsClassMetadata) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  ASSERT_EQ(classes->size(), 1u);
  EXPECT_EQ((*classes)[0].id, "double_it");
  EXPECT_EQ((*classes)[0].output_kind, "double");
}

TEST(PythonEngine, CalculateComputesValueTimesTwo) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule(kDoubleSource, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  auto r = (*inst)->calculate(0.0, 21.0);
  EXPECT_FALSE(r.suppress);
  EXPECT_DOUBLE_EQ(r.value, 42.0);
  EXPECT_FALSE((*inst)->failed());
}

TEST(PythonEngine, SyntaxErrorIsReported) {
  auto engine = makePythonEngine();
  auto classes = engine->inspectModule("# pj-script: python\nclass T\n  bad", "test");
  EXPECT_FALSE(classes.has_value());
}

TEST(PythonEngine, RuntimeErrorFailsStickily) {
  auto engine = makePythonEngine();
  constexpr const char* kBad = R"PY(# pj-script: python
class T:
    id = "boom"
    @staticmethod
    def create(params):
        return T()
    def calculate(self, time, value, *args):
        return value + "oops"
)PY";
  auto classes = engine->inspectModule(kBad, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance((*classes)[0], "{}");
  ASSERT_TRUE(inst.has_value());
  auto r = (*inst)->calculate(0.0, 1.0);
  EXPECT_TRUE(r.suppress);         // a runtime error suppresses output
  EXPECT_TRUE((*inst)->failed());  // ...and puts the instance into a sticky failed state
}
