// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace PJ::scripting {

/// Parameter widget kinds the generated `ParameterForm` understands. The schema
/// is declarative data (read off a filter class's `parameters` table) so the
/// host builds the editor panel without any per-filter Qt. See
/// pj_scripting/docs/FILTER_CLASS.md for the authoritative schema contract.
enum class ParamType {
  kNumber,   ///< double  -> QDoubleSpinBox / validated line edit
  kInteger,  ///< int64   -> QSpinBox
  kBoolean,  ///< bool    -> CheckButton / ToggleSwitch
  kEnum,     ///< choice  -> PJ::ComboBox (stored value is the option's string)
  kString,   ///< text    -> QLineEdit (single line)
  kText,     ///< text    -> QPlainTextEdit (multi-line, e.g. a custom expression)
};

/// One enum option: the value persisted in params JSON plus its UI label. The
/// value is TYPED (a JSON scalar) so an int-keyed enum (e.g. binary_filter's
/// `binary_op`) round-trips as a number, not the string "3".
struct EnumValue {
  nlohmann::json value;
  std::string label;
};

/// One class-level parameter, parsed from a filter class's `parameters` entry
/// WITHOUT instantiating the filter. Drives both the catalogue entry and the
/// generated panel; `name` is also the JSON key in `saveParams()`/`loadParams()`.
struct ParamSpec {
  std::string name;  ///< params-table key + JSON key (required)
  ParamType type = ParamType::kNumber;
  std::string label;  ///< UI label; falls back to `name` when empty
  std::string tooltip;
  nlohmann::json default_value;          ///< typed default (number/int/bool/string)
  std::optional<double> min, max, step;  ///< numeric clamp / spin step
  std::optional<int> decimals;           ///< double display precision
  std::string unit;                      ///< suffix shown after the value, e.g. "s"
  std::vector<EnumValue> values;         ///< enum options (kEnum only)

  /// Optional conditional visibility: show this row only when parameter
  /// `visible_when_param` currently equals `visible_when_equals`.
  std::optional<std::string> visible_when_param;
  nlohmann::json visible_when_equals;
};

/// A self-describing filter class read off a `.luau` module, WITHOUT running
/// `create()`/`calculate()`. `source` is the full module text (so an instance
/// can be built later, and layouts can embed it for portability); `origin`
/// labels where it came from (e.g. "bundled").
struct FilterClass {
  std::string id;    ///< stable catalogue key (e.g. "derivative")
  std::string name;  ///< human type name
  std::string description;
  std::string version;
  std::string output_kind = "double";  ///< "double" | "int64" | "same"
  std::vector<ParamSpec> parameters;
  std::string source;  ///< full module text the class was parsed from
  std::string origin;  ///< provenance label (e.g. "bundled")
};

}  // namespace PJ::scripting
