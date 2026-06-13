#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

namespace PJ {

class ISceneLayer;

/// Serializes a layer's user-tunable parameters — its `xmlSaveState` element —
/// to a standalone XML string, for copy/paste of settings between layers of the
/// same family. Returns an empty string when the layer exposes no serializable
/// state (a base layer whose `xmlSaveState` yields a null element).
[[nodiscard]] QString serializeLayerParams(const ISceneLayer& layer);

/// Applies parameters produced by `serializeLayerParams()` to `layer` through its
/// `xmlLoadState`. The layer's setters emit `repaintRequested()`, so the view
/// refreshes on its own — callers need not trigger a repaint. Returns false when
/// `xml` is empty or not parseable.
///
/// The caller owns any family/type-compatibility decision: this applies the blob
/// as-is, and a layer simply ignores attributes it does not recognize. Pasting a
/// blob from a different family is therefore safe but a no-op, which is why the UI
/// gates the paste action on matching `SceneLayerInfo::family_name`.
[[nodiscard]] bool applyLayerParams(ISceneLayer& layer, const QString& xml);

}  // namespace PJ
