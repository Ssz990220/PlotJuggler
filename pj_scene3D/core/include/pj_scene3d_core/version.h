#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace pj::scene3d {
// Placeholder phase stamp — vestigial scaffolding. kPhase is not consumed by
// any build step, runtime gate, or ABI check; its only reference is the
// static_assert in version.cpp. The core now has real translation units
// (pointcloud_codecs.cpp, scene_entities_decode.cpp, tf/*.cpp). If a real
// version/ABI stamp is needed later, replace this with something that is
// actually read (e.g. a layer-factory capability gate or a schema version).
inline constexpr int kPhase = 1;
}  // namespace pj::scene3d
