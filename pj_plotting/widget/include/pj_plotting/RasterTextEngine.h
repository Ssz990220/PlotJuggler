// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <qwt_text_engine.h>

#include <QImage>
#include <memory>

class QPainter;

namespace PJ {

/**
 * Decorator around a stock Qwt text engine that makes on-canvas text survive
 * GPU resets and GL context recreation.
 *
 * Qt's OpenGL paint engine renders glyphs from a per-context atlas texture
 * whose contents are assumed resident forever. A GPU reset (driver hang
 * recovery, suspend/resume) or an ADS-reparent context recreation leaves that
 * atlas blank, so every already-cached glyph draws as nothing while vector
 * primitives — re-tessellated per frame — keep rendering. Observable symptom:
 * legend/tracker text vanishes while curves, dots, and boxes stay (the same
 * failure class pj_scene3D's HUD hit; fixed there by CPU-raster too).
 *
 * draw() therefore routes GL-targeted text through a freshly allocated CPU
 * QImage (rasterized by the wrapped engine) and blits it with drawImage().
 * A fresh image per draw carries a new cacheKey, forcing the GL engine to
 * re-upload its texture — deliberately NOT cached, because any resident GL
 * texture (glyph atlas or cached image) is a GPU-reset casualty. Non-GL
 * paint devices pass straight through to the wrapped engine.
 *
 * Layout queries (textSize/heightForWidth/textMargins/mightRender) delegate
 * unchanged, so alignment and wrapping behave exactly like the stock engine.
 */
class RasterTextEngine : public QwtTextEngine {
 public:
  /// Takes ownership of the stock engine used for layout and CPU rasterization.
  explicit RasterTextEngine(std::unique_ptr<QwtTextEngine> inner);
  ~RasterTextEngine() override;

  double heightForWidth(const QFont& font, int flags, const QString& text, double width) const override;
  QSizeF textSize(const QFont& font, int flags, const QString& text) const override;
  bool mightRender(const QString& text) const override;
  void textMargins(
      const QFont& font, const QString& text, double& left, double& right, double& top, double& bottom) const override;
  void draw(QPainter* painter, const QRectF& rect, int flags, const QString& text) const override;

  /// Process-wide count of draws that took the CPU-raster GL path. Diagnostic
  /// companion to PJ_PLOT_TEXT_DEBUG; also lets tests prove routing happened.
  static int glRasterDrawCount();

 private:
  std::unique_ptr<QwtTextEngine> inner_;
};

/// True when the painter targets an OpenGL paint engine, i.e. text drawn
/// through it would go via the fragile per-context glyph atlas.
bool painterUsesGlTextPath(const QPainter* painter);

/**
 * Rasterize `text` with `engine` into a transparent, DPR-aware image.
 *
 * Pen, font, and render hints are copied from `state_source`; the image is
 * allocated at logical_size × the effective pixel ratio — the source device's
 * devicePixelRatio *times the painter transform's scale* (Qwt's GL backing
 * store paints on a DPR-1 physical-size device and carries the display ratio
 * as a painter scale) — and tagged with that ratio, so the caller's
 * drawImage() at logical coordinates stays crisp on HiDPI. Returns a null
 * image for empty text or a degenerate size.
 */
QImage rasterizeEngineText(
    const QwtTextEngine& engine, const QPainter& state_source, const QSizeF& logical_size, int flags,
    const QString& text);

/**
 * Replace Qwt's global PlainText/RichText engines with RasterTextEngine
 * wrappers. Must run before ANY QwtText is constructed: Qwt deletes the
 * replaced engines while existing QwtText objects cache raw pointers to them
 * (they would dangle on their next draw). Call at the top of main().
 * Idempotent — returns true on first installation, false afterwards.
 */
bool installRasterTextEngines();

}  // namespace PJ
