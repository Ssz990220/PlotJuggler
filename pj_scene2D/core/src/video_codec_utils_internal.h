#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>

namespace PJ {

// Offset of the byte after the next Annex-B start code at/after `offset`, or
// `size` if none. Start codes are 0x000001 (3-byte) or 0x00000001 (4-byte).
size_t nextNalHeader(const uint8_t* data, size_t size, size_t offset);

}  // namespace PJ
