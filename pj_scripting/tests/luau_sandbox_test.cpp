// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include "pj_scripting/script_engine.h"

using namespace PJ::scripting;

// A filter that returns 1 only if the dangerous globals are all absent.
TEST(LuauSandbox, DangerousGlobalsAreAbsent) {
  constexpr const char* kProbe = R"LUAU(
    return { id="probe", name="P",
      create = function(p) return { calculate = function(t, v)
        if io == nil and os == nil and loadstring == nil and dofile == nil and require == nil
          and setfenv == nil and getfenv == nil then
          return 1
        else
          return 0
        end
      end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kProbe, "test");
  ASSERT_TRUE(classes.has_value()) << (classes.has_value() ? "" : classes.error());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value()) << (inst.has_value() ? "" : inst.error());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 0.0).value, 1.0);
}

// The stdlib is frozen read-only: a filter cannot monkey-patch math.
TEST(LuauSandbox, StdlibMutationIsRejected) {
  constexpr const char* kEvil = R"LUAU(
    return { id="evil", name="E",
      create = function(p) math.floor = nil; return { calculate = function(t, v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kEvil, "test");
  ASSERT_TRUE(classes.has_value());  // inspect never runs create
  auto inst = engine->createInstance(classes->front(), "{}");
  EXPECT_FALSE(inst.has_value());  // create() writes a read-only stdlib table → error
}

// The global table is frozen: a module cannot leak a new global.
TEST(LuauSandbox, GlobalWriteInModuleIsRejected) {
  constexpr const char* kGlobalWrite = R"LUAU(
    sneaky = 42
    return { id="x", name="X", create = function(p) return { calculate = function(t, v) return v end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  EXPECT_FALSE(engine->inspectModule(kGlobalWrite, "test").has_value());
}

// A malicious __index metamethod must NOT run during host-side inspection (raw
// field reads): otherwise a looping __index would hang/crash the app outside a
// protected call. Raw reads see no `id` → a clean error, no hang.
TEST(LuauSandbox, MetatableIndexCannotHangInspection) {
  constexpr const char* kEvil = R"LUAU(
    return setmetatable({}, { __index = function() while true do end end })
  )LUAU";
  auto engine = makeLuauEngine();
  EXPECT_FALSE(engine->inspectModule(kEvil, "test").has_value());  // must return, never hang
}

// A legitimate filter (locals + returned tables, no globals) works under the sandbox.
TEST(LuauSandbox, LegitimateFilterStillWorks) {
  constexpr const char* kScale = R"LUAU(
    return { id="scale", name="S",
      parameters = { { name="k", type="number", default=3.0 } },
      create = function(p) local k = p.k; return { calculate = function(t, v) return v * k end } end }
  )LUAU";
  auto engine = makeLuauEngine();
  auto classes = engine->inspectModule(kScale, "test");
  ASSERT_TRUE(classes.has_value());
  auto inst = engine->createInstance(classes->front(), "{}");
  ASSERT_TRUE(inst.has_value());
  EXPECT_DOUBLE_EQ((*inst)->calculate(0.0, 4.0).value, 12.0);
}
