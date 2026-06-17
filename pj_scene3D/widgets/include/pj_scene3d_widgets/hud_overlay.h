// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QStringList>

namespace pj::scene3d {

// Rasterize a translucent, rounded HUD panel holding `lines` (left-aligned,
// stacked top to bottom) into a freshly-allocated QImage and return it. The
// image is allocated at `device_pixel_ratio` physical resolution and tagged
// with that ratio, so a caller can `painter.drawImage(logical_top_left, img)`
// and get a crisp result at any DPR; the panel's logical box is the image's
// device-independent size (`img.deviceIndependentSize()`).
//
// `padding` is the logical inset between the panel edge and the text on every
// side; `panel_alpha` is the 0..255 opacity of the black panel fill; lines are
// drawn in `text_color`. Returns a NULL QImage when there is nothing to draw
// (no lines, or every line empty), so the caller can skip the blit.
//
// Why this exists — the perf HUD and the TF hover label must NOT paint their
// text with `QPainter` bound directly to the `SceneViewWidget` (a
// `QOpenGLWidget`). That path caches glyphs in a per-GL-context texture atlas,
// and this module deliberately runs each 3D view on its own unshared GL context
// (no `Qt::AA_ShareOpenGLContexts`), so the context is *recreated* whenever ADS
// reparents the dock — dock/float/split, and notably **layout restore**, which
// reparents the view into the reconstructed dock tree. After such a recreation
// the glyph atlas is left stale / built at the wrong device-pixel ratio, so
// text renders doubled/garbled while vector fills (the panel itself) stay
// correct. Rasterizing the text on the CPU here and blitting the result with
// `drawImage()` — a plain textured quad that never touches the glyph atlas — is
// immune to the context recreation and to later DPR changes.
QImage renderHudPanel(
    const QStringList& lines, const QFont& font, qreal device_pixel_ratio, int padding, int panel_alpha,
    const QColor& text_color);

}  // namespace pj::scene3d
