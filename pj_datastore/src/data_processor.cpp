// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_datastore/data_processor.hpp"

#include <utility>

namespace PJ::proc {

void DataProcessor::appendTail(const std::vector<Sample>& tail, std::vector<Sample>& out) {
  out.reserve(out.size() + tail.size());
  for (const Sample& in : tail) {
    if (std::optional<Sample> result = calculateNextPoint(in)) {
      out.push_back(std::move(*result));
    }
  }
}

std::vector<Sample> DataProcessor::applyBatch(const std::vector<Sample>& in) {
  reset();
  std::vector<Sample> out;
  appendTail(in, out);
  return out;
}

std::vector<PJ::StorageKind> DataProcessor::outputKinds(PJ::Span<const PJ::StorageKind> in) const {
  (void)in;
  return std::vector<PJ::StorageKind>(static_cast<std::size_t>(numOutputs()), PJ::StorageKind::kFloat64);
}

}  // namespace PJ::proc
