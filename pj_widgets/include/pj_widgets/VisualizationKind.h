#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

namespace PJ {

// Which visualization family a placeholder icon offers. A neutral UI vocabulary
// owned by pj_widgets: `Plot` is the Qwt time-series plot; `Scene2D` / `Scene3D`
// are mapped to the concrete object-widget "kinds" ("scene2d"/"scene3d") by the
// app shell (the only module that knows scene families), so pj_widgets and
// pj_plotting stay agnostic to them.
enum class VisualizationKind { kPlot, kScene2D, kScene3D };

}  // namespace PJ
