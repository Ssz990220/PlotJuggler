// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/RasterTextEngine.h"

#include <qwt_text.h>

#include <QPaintEngine>
#include <QPainter>
#include <atomic>
#include <cmath>
#include <utility>

namespace PJ {

namespace {
std::atomic<int> g_gl_raster_draw_count{0};
}  // namespace

RasterTextEngine::RasterTextEngine(std::unique_ptr<QwtTextEngine> inner) : inner_(std::move(inner)) {}

RasterTextEngine::~RasterTextEngine() = default;

double RasterTextEngine::heightForWidth(const QFont& font, int flags, const QString& text, double width) const {
  return inner_->heightForWidth(font, flags, text, width);
}

QSizeF RasterTextEngine::textSize(const QFont& font, int flags, const QString& text) const {
  return inner_->textSize(font, flags, text);
}

bool RasterTextEngine::mightRender(const QString& text) const {
  return inner_->mightRender(text);
}

void RasterTextEngine::textMargins(
    const QFont& font, const QString& text, double& left, double& right, double& top, double& bottom) const {
  inner_->textMargins(font, text, left, right, top, bottom);
}

void RasterTextEngine::draw(QPainter* painter, const QRectF& rect, int flags, const QString& text) const {
  if (!painterUsesGlTextPath(painter)) {
    inner_->draw(painter, rect, flags, text);
    return;
  }
  const QImage image = rasterizeEngineText(*inner_, *painter, rect.size(), flags, text);
  if (image.isNull()) {
    return;
  }
  g_gl_raster_draw_count.fetch_add(1, std::memory_order_relaxed);
  painter->drawImage(rect.topLeft(), image);
}

int RasterTextEngine::glRasterDrawCount() {
  return g_gl_raster_draw_count.load(std::memory_order_relaxed);
}

bool painterUsesGlTextPath(const QPainter* painter) {
  if (painter == nullptr || !painter->isActive()) {
    return false;
  }
  const QPaintEngine* engine = painter->paintEngine();
  return engine != nullptr && (engine->type() == QPaintEngine::OpenGL2 || engine->type() == QPaintEngine::OpenGL);
}

QImage rasterizeEngineText(
    const QwtTextEngine& engine, const QPainter& state_source, const QSizeF& logical_size, int flags,
    const QString& text) {
  if (text.isEmpty() || logical_size.width() < 1.0 || logical_size.height() < 1.0) {
    return {};
  }
  const QPaintDevice* device = state_source.device();
  const qreal device_dpr = (device != nullptr && device->devicePixelRatioF() > 0.0) ? device->devicePixelRatioF() : 1.0;
  // Qwt's GL backing store paints on a physical-size QOpenGLPaintDevice (DPR 1)
  // and applies the display ratio as a painter *scale* — fold the transform's
  // scale in, or HiDPI text rasters at 1x and blits up blurry.
  const QTransform& transform = state_source.transform();
  const qreal transform_scale = std::hypot(transform.m11(), transform.m12());
  const qreal dpr = device_dpr * (transform_scale > 0.0 ? transform_scale : 1.0);
  const QSize physical(
      static_cast<int>(std::lround(logical_size.width() * dpr)),
      static_cast<int>(std::lround(logical_size.height() * dpr)));
  if (physical.isEmpty()) {
    return {};
  }

  QImage image(physical, QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(dpr);
  image.fill(Qt::transparent);

  QPainter raster_painter(&image);
  raster_painter.setRenderHints(state_source.renderHints());
  raster_painter.setRenderHint(QPainter::TextAntialiasing, true);
  raster_painter.setPen(state_source.pen());
  raster_painter.setFont(state_source.font());
  engine.draw(&raster_painter, QRectF(QPointF(0.0, 0.0), logical_size), flags, text);
  raster_painter.end();
  return image;
}

bool installRasterTextEngines() {
  static bool installed = false;
  if (installed) {
    return false;
  }
  installed = true;
  QwtText::setTextEngine(QwtText::PlainText, new RasterTextEngine(std::make_unique<QwtPlainTextEngine>()));
  QwtText::setTextEngine(QwtText::RichText, new RasterTextEngine(std::make_unique<QwtRichTextEngine>()));
  return true;
}

}  // namespace PJ
