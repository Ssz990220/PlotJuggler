// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene3d_core/version.h"
namespace pj::scene3d {
// Vestigial scaffolding: kPhase has no real consumer (see version.h). This TU
// exists only to anchor the self-assert until a real version surface lands.
static_assert(kPhase == 1, "Phase mismatch");
}  // namespace pj::scene3d
