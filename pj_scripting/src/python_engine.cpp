// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Embedded-CPython backend for Data Processors (MVP). Implements the language-
// agnostic ScriptEngine seam with pybind11's embedding API. See python_engine.h
// for the Python "filter module" contract and the MVP caveats.

#include "pj_scripting/python_engine.h"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "pj_base/sdk/platform.hpp"

namespace py = pybind11;

namespace PJ::scripting {
namespace {

// Lazily start the one process-wide CPython interpreter. Both the guard and the
// GIL-release are intentionally leaked: finalizing embedded CPython at static
// destruction is fragile, and releasing the GIL lets every entry point re-acquire
// it with py::gil_scoped_acquire from whatever thread calls.
void ensureInterpreter() {
  static const bool inited = []() {
#ifdef PJ_PYTHON_HOME
    // The embedded interpreter must find the stdlib (encodings, etc.) at startup.
    // Point it at the CPython prefix baked at build time, unless the user already
    // set PYTHONHOME. Dev-oriented: deployment would bundle Python.
    // setenv is POSIX-only (MSVC lacks it); use _putenv_s on Windows. Set only if
    // absent, mirroring setenv's overwrite=0 — sdk::getEnv is the portable read.
    if (!sdk::getEnv("PYTHONHOME")) {
#ifdef _WIN32
      _putenv_s("PYTHONHOME", PJ_PYTHON_HOME);
#else
      ::setenv("PYTHONHOME", PJ_PYTHON_HOME, 1);
#endif
    }
#endif
    new py::scoped_interpreter();  // NOLINT: leaked on purpose (never finalize)
    new py::gil_scoped_release();  // NOLINT: leaked on purpose (GIL handed to callers)
    return true;
  }();
  (void)inited;
}

std::string attrString(const py::object& obj, const char* name, const std::string& fallback) {
  if (!py::hasattr(obj, name)) {
    return fallback;
  }
  py::object v = obj.attr(name);
  if (v.is_none()) {
    return fallback;
  }
  return py::str(v).cast<std::string>();
}

// One live Python filter: owns the module namespace (keepalive) and the instance
// object. All py-object touches happen under the GIL.
class PythonFilterInstance final : public FilterInstance {
 public:
  PythonFilterInstance(std::shared_ptr<py::object> ns, py::object instance)
      : ns_(std::move(ns)), instance_(std::move(instance)) {}

  ~PythonFilterInstance() override {
    py::gil_scoped_acquire gil;
    instance_ = py::object();
    ns_.reset();
  }

  PythonFilterInstance(const PythonFilterInstance&) = delete;
  PythonFilterInstance& operator=(const PythonFilterInstance&) = delete;

  Result calculate(double t, double v) override {
    if (failed_) {
      return Result{true, 0.0, std::nullopt};
    }
    py::gil_scoped_acquire gil;
    try {
      py::object r = instance_.attr("calculate")(t, v);
      if (r.is_none()) {
        return Result{true, 0.0, std::nullopt};
      }
      // (out_time, value) when the script returns a 2-sequence; otherwise a scalar
      // emitted at the input time (mirrors the Luau one/two-number convention).
      if (py::isinstance<py::tuple>(r) || py::isinstance<py::list>(r)) {
        auto seq = r.cast<py::sequence>();
        if (seq.size() == 2) {
          return Result{false, seq[1].cast<double>(), seq[0].cast<double>()};
        }
        if (seq.size() == 1) {
          return Result{false, seq[0].cast<double>(), std::nullopt};
        }
        fail("calculate() returned a sequence of unexpected length (want 1 or 2)");
        return Result{true, 0.0, std::nullopt};
      }
      return Result{false, r.cast<double>(), std::nullopt};
    } catch (const py::error_already_set& e) {
      fail(e.what());
      return Result{true, 0.0, std::nullopt};
    } catch (const std::exception& e) {
      fail(e.what());
      return Result{true, 0.0, std::nullopt};
    }
  }

  MimoResult calculateMimo(double t, PJ::Span<const double> inputs) override {
    if (failed_) {
      return MimoResult{true, {}};
    }
    py::gil_scoped_acquire gil;
    try {
      py::list args;
      for (std::size_t i = 1; i < inputs.size(); ++i) {
        args.append(inputs[i]);
      }
      const double primary = inputs.empty() ? 0.0 : inputs[0];
      py::object r = instance_.attr("calculate")(t, primary, *py::tuple(args));
      if (r.is_none()) {
        return MimoResult{true, {}};
      }
      MimoResult out;
      out.suppress = false;
      if (py::isinstance<py::tuple>(r) || py::isinstance<py::list>(r)) {
        for (auto item : r.cast<py::sequence>()) {
          out.values.push_back(py::reinterpret_borrow<py::object>(item).cast<double>());
        }
      } else {
        out.values.push_back(r.cast<double>());
      }
      return out;
    } catch (const py::error_already_set& e) {
      fail(e.what());
      return MimoResult{true, {}};
    } catch (const std::exception& e) {
      fail(e.what());
      return MimoResult{true, {}};
    }
  }

  void reset() override {
    if (failed_) {
      return;
    }
    py::gil_scoped_acquire gil;
    if (py::hasattr(instance_, "reset")) {
      try {
        instance_.attr("reset")();
      } catch (const py::error_already_set& e) {
        fail(e.what());
      }
    }
  }

  [[nodiscard]] bool failed() const override {
    return failed_;
  }
  [[nodiscard]] const std::string& error() const override {
    return error_;
  }

 private:
  void fail(std::string msg) {
    failed_ = true;
    error_ = std::move(msg);
  }

  std::shared_ptr<py::object> ns_;  // module namespace dict, kept alive
  py::object instance_;
  bool failed_ = false;
  std::string error_;
};

class PythonEngine final : public ScriptEngine {
 public:
  Expected<std::vector<FilterClass>> inspectModule(const std::string& source, const std::string& origin) override {
    ensureInterpreter();
    py::gil_scoped_acquire gil;
    try {
      py::dict ns;
      py::exec(source, ns);
      if (!ns.contains("T")) {
        return PJ::unexpected("Python filter module must define a top-level class named 'T'");
      }
      py::object cls = ns["T"];
      if (!py::hasattr(cls, "id")) {
        return PJ::unexpected("Python filter class 'T' must define an 'id'");
      }
      FilterClass fc;
      fc.id = attrString(cls, "id", "");
      if (fc.id.empty()) {
        return PJ::unexpected("Python filter class 'T' has an empty 'id'");
      }
      fc.name = attrString(cls, "name", fc.id);
      fc.description = attrString(cls, "description", "");
      fc.version = attrString(cls, "version", "");
      fc.output_kind = attrString(cls, "output", "double");
      fc.source = source;
      fc.origin = origin;
      return std::vector<FilterClass>{std::move(fc)};
    } catch (const py::error_already_set& e) {
      return PJ::unexpected(std::string("Python error: ") + e.what());
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("Python error: ") + e.what());
    }
  }

  Expected<std::unique_ptr<FilterInstance>> createInstance(
      const FilterClass& klass, const std::string& params_json) override {
    ensureInterpreter();
    py::gil_scoped_acquire gil;
    try {
      auto ns = std::make_shared<py::object>(py::dict());
      py::exec(klass.source, *ns);
      py::dict& nsd = reinterpret_cast<py::dict&>(*ns);
      if (!nsd.contains("T")) {
        return PJ::unexpected("Python filter module must define a top-level class named 'T'");
      }
      py::object cls = nsd["T"];
      py::object params;
      if (params_json.empty() || params_json == "{}") {
        params = py::dict();
      } else {
        params = py::module_::import("json").attr("loads")(params_json);
      }
      if (!py::hasattr(cls, "create")) {
        return PJ::unexpected("Python filter class 'T' must define a static create(params)");
      }
      py::object instance = cls.attr("create")(params);
      if (!py::hasattr(instance, "calculate")) {
        return PJ::unexpected("the object returned by T.create() must have a calculate(self, time, value) method");
      }
      return std::unique_ptr<FilterInstance>(
          std::make_unique<PythonFilterInstance>(std::move(ns), std::move(instance)));
    } catch (const py::error_already_set& e) {
      return PJ::unexpected(std::string("Python error: ") + e.what());
    } catch (const std::exception& e) {
      return PJ::unexpected(std::string("Python error: ") + e.what());
    }
  }
};

}  // namespace

std::shared_ptr<ScriptEngine> makePythonEngine(BudgetLimits /*limits*/) {
  return std::make_shared<PythonEngine>();
}

}  // namespace PJ::scripting
