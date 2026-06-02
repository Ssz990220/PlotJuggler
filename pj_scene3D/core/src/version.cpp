// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/version.h"
namespace pj::scene3d {
// Reserve a real TU for Phase 1's version stamp. Future translation units
// land alongside as the core grows in T2+.
static_assert(kPhase == 1, "Phase mismatch");
}  // namespace pj::scene3d
