// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>

#include "pj_base/builtin/builtin_object.hpp"

namespace PJ {

class CodecPipeline;

/// Built-in parser-less decode policy for 2D scene layers.
[[nodiscard]] std::unique_ptr<CodecPipeline> makeScene2DPipelineFor(sdk::BuiltinObjectType object_type);

}  // namespace PJ
