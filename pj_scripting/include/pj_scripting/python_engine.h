// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "pj_scripting/sandbox.h"
#include "pj_scripting/script_engine.h"

namespace PJ::scripting {

/// Construct the embedded-CPython backend (MVP). Implements the same `ScriptEngine`
/// seam as `makeLuauEngine`, so a `FilterCatalogue` built with it produces the same
/// `LuaSisoTransform`/`LuaMimoTransform` nodes — no new streaming plumbing.
///
/// A Python "filter module" defines a top-level class named `T`:
///
///     # pj-script: python
///     class T:
///         id = "my_filter"
///         name = "My Filter"
///         output = "double"            # "double" | "int64" | "same"
///         @staticmethod
///         def create(params):          # params: dict from params_json
///             return T()
///         def calculate(self, time, value, *args):
///             return value * 2         # number, or (out_time, value), or None to suppress
///
/// MVP CAVEATS (vs the Luau backend): single process-wide interpreter (one GIL,
/// commit/GUI thread only); no watchdog / weak sandbox (a runaway script can hang
/// the app, and module-level code runs during inspection). `limits` is accepted
/// for API symmetry but not yet enforced.
[[nodiscard]] std::shared_ptr<ScriptEngine> makePythonEngine(BudgetLimits limits = {});

}  // namespace PJ::scripting
