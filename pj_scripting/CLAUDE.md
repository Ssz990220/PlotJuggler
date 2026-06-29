# pj_scripting — Luau filter engine for Data Processors

The scripting substrate for PJ4 **Data Processors**. Filters are **self-describing
`.luau` classes**: each declares its `id/name/description/version/output_kind` and a
typed `parameters` schema, plus a `create()`/`calculate()` (and optional
`calculate_batch`) implementation. The host reads that schema to (a) populate a
filter **catalogue** and (b) **generate** the parameter-editor form at runtime — so
changing a filter is a data edit, not a panel-code recompile. The engine is hidden
behind a thin **`ScriptEngine`** seam (`include/pj_scripting/script_engine.h`,
constructed via free fn `makeLuauEngine(BudgetLimits = {})`) so a future
out-of-process Python backend is a drop-in. The binding is **hand-written Luau**
(`src/luau_engine.cpp`) — Luau's API is C++ linkage, not `extern "C"`, so sol2 does
not apply. The data-processor base (`PJ::proc::DataProcessor`, owned by `pj_datastore`)
stays Qt-free **and Luau-free**: only `pj_scripting` links Luau.
Depends on `pj_base` + `pj_datastore` (public) + Luau (private). Licensed MPL-2.0.

## Key headers

- `script_engine.h` — the `ScriptEngine` seam + `FilterInstance` (one VM per
  instance) + `makeLuauEngine(BudgetLimits)`.
- `filter_class.h` — the `FilterClass` schema + `ParamSpec`
  (`id/name/description/version/output_kind/parameters/source/origin`; `ParamType` ∈
  {number, integer, boolean, enum, string, text}). Drives the generated
  ParameterForm and the catalogue. The full schema/form contract is in
  [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md).
- `filter_catalogue.h` — `FilterCatalogue`, a Qt-free registry. v1 loads the single
  bundled multi-class resource via `addBundledSource`; plus `find`, `entries`,
  `makeProcessor`, and `makeProcessorFromSource` (embedded-source restore). A
  user-override dir + marketplace install are DEFERRED.
- `lua_siso_transform.h` — `LuaSisoTransform`, a `PJ::proc::DataProcessor` whose
  `calculateNextPoint` runs a Luau filter class via the `ProcessorSisoAdapter` as an
  eager `pj_datastore::DerivedEngine` node.
- `sandbox.h` — `BudgetLimits` (the per-VM sandbox/watchdog budget).

## Contract / gotchas

- **Sandbox + watchdog (`sandbox.h` `BudgetLimits`).** Each filter VM runs
  IN-PROCESS on the commit thread, so it is hardened: `luaL_sandbox` freezes the
  shared stdlib read-only, then `luaL_sandboxthread` gives the VM its own writable
  global table layered on the frozen stdlib (read-only `__index`). Net effect: a
  filter can assign top-level globals for persistent state, but cannot mutate the
  stdlib or leak globals into a sibling filter's VM. Plus a custom `lua_Alloc`
  memory cap and an interrupt-based instruction-budget watchdog armed per protected
  region. `os` / `io` / `require` / `debug` / `setfenv` / `getfenv` are blocked
  (`setfenv` would let a script swap its environment back onto the frozen stdlib).
  Each VM loads exactly ONE module, so `safeenv` stays on and stdlib imports keep
  the fast path. A "tripped" flag is checked by the host **even if the script
  `pcall`-swallows the error**, so a runaway can never silently survive.
- **Time contract (precision-critical).** The script sees `t` = **seconds since
  session start**. The int64-ns session-start is subtracted on the absolute spine
  **before** the double cast (avoiding epoch-double quantization, ~256 ns at epoch
  magnitude); output timestamps are reconstructed onto the absolute int64 spine. Get
  this wrong and every `dt`-based filter quantizes.
- **`LuaSisoTransform` failure mode.** A script runtime error **fails the node**
  (sticky), never corrupting a stateful accumulator. `reset()` =
  construct-new-and-swap. Luau filters are numeric-scalar only: a string-valued
  input is silently coerced to `0.0` (`proc::detail::toDouble`), not rejected —
  in practice series fed to a filter are numeric, so this case does not arise.
- **Filters are DATA.** `resources/filters/builtin_filters.luau` is ONE `.luau`
  resource returning a list of 12 classes (none, absolute, scale, derivative,
  integral, moving_average, moving_rms, moving_variance, outlier_removal,
  samples_counter, binary_filter, time_since_previous). The app reads the QRC
  resource and hands the source string to `addBundledSource` — `pj_scripting` never
  touches Qt. v1 loads only this bundled resource.

## Read deeper

| For | Read |
|---|---|
| The filter-class + ParameterForm schema contract, sandbox, time rebasing | [`docs/FILTER_CLASS.md`](./docs/FILTER_CLASS.md) |
| The processor base this implements | `../pj_datastore/include/pj_datastore/data_processor.hpp` |
