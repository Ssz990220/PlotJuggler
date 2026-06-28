// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pj_base/builtin/builtin_object.hpp"  // BuiltinObjectType
#include "pj_base/sdk/plugin_data_api.hpp"     // ObjectRecord, PayloadView
#include "pj_base/types.hpp"                   // Timestamp, Expected
#include "pj_runtime/SessionManager.h"         // SessionManager::ParserBinding

namespace pj::scene3d {

/// True when `type` has a pj_base canonical wire codec that resolveObject() can
/// decode WITHOUT a MessageParser. Lets a consumer's attach()-time gate accept a
/// parser-less (canonical) object topic instead of rejecting it for "no parser".
///
/// This reports DECODE capability (a pj_base codec exists), NOT that a render
/// layer consumes the type via the canonical path — it returns true for more
/// types than are currently wired (only PointCloudLayer, PosesInFrameLayer and
/// the TF ingest use it today; OccupancyGrid/VoxelGrid/SceneEntities layers still
/// require a parser). A caller using it as an attach gate must therefore already
/// know its own layer renders `type`; do not read a true result as "some layer
/// will render this".
[[nodiscard]] bool hasCanonical3DCodec(PJ::sdk::BuiltinObjectType type) noexcept;

/// Decode one ObjectStore sample into an ObjectRecord, choosing the decode path
/// the way every 3D consumer must:
///
///  - **Parser bound** (file / streaming data sources whose objects arrive as
///    parser-decoded messages): route through the topic's MessageParser via
///    parseLocked(). `type` is ignored.
///  - **No parser bound** (a data-source / toolbox that pushed an already
///    *serialized canonical* object — e.g. the Mosaico cloud toolbox): the bytes
///    are a pj_base wire blob, so deserialize them with the canonical codec
///    selected by `type` (the topic's `builtin_object_type` metadata).
///
/// This mirrors pj_scene2D's `canonical = (binding.parser == nullptr)`
/// discipline: the HOST always performs the decode; the producing plugin never
/// does. The returned object owns/anchors its own bytes (the canonical codecs
/// deep-copy; parsers anchor the payload), so callers may use it after `binding`
/// drops — see parse_locked.h.
///
/// On the canonical path `ObjectRecord::ts` is left nullopt so the host falls
/// back to the store entry's timestamp (matching the canonical image path);
/// per-element stamps (e.g. FrameTransforms) are read by the consumer from the
/// decoded struct itself. Returns an error when no parser is bound and `type`
/// has no canonical codec here.
[[nodiscard]] PJ::Expected<PJ::sdk::ObjectRecord> resolveObject(
    const PJ::SessionManager::ParserBinding& binding, PJ::sdk::BuiltinObjectType type, PJ::Timestamp ts,
    const PJ::sdk::PayloadView& payload);

}  // namespace pj::scene3d
