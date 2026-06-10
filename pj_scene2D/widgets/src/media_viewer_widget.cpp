// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/media_viewer_widget.h"

#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMetaObject>
#include <QPainter>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "pj_scene2d_core/media_source.h"
#include "pj_scene2d_widgets/pixel_inspector.h"

void pjMediaQtInitResources() {
  Q_INIT_RESOURCE(shaders);
}

namespace PJ {

// BT.709 color matrix (HD video standard)
// clang-format off
static constexpr float kBT709[] = {
    1.0f,    1.0f,      1.0f,    0.0f,
    0.0f,   -0.18732f,  1.8556f, 0.0f,
    1.5748f, -0.46812f,  0.0f,   0.0f,
    0.0f,    0.0f,      0.0f,    1.0f
};
// clang-format on

static constexpr int kPointInspectorCropSize = 10;

MediaViewerWidget::MediaViewerWidget(QWidget* parent) : QRhiWidget(parent) {
  setApi(Api::OpenGL);
  setObjectName(QStringLiteral("mediaViewerCanvas"));
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  static bool resources_initialized = [] {
    pjMediaQtInitResources();
    return true;
  }();
  (void)resources_initialized;
}

MediaViewerWidget::~MediaViewerWidget() {
  // Qt does NOT call releaseResources() on widget destruction — only when the QRhi
  // context changes (reparent / window move). Without this, every GPU resource
  // (the pipelines, textures, buffers, samplers, SRBs, glyph cache and pixel-layer
  // textures) leaks each time a dock destroys a viewer. releaseResources() is
  // idempotent (it nulls each pointer), and the QRhiWidget base that owns the QRhi
  // is destroyed AFTER this derived destructor, so the rhi is still alive here.
  releaseResources();
}

void MediaViewerWidget::setClearColor(const QColor& color) {
  QColor next = color.isValid() ? color : QColor(Qt::white);
  next.setAlpha(255);
  if (next == clear_color_) {
    return;
  }
  clear_color_ = next;
  update();
}

QColor MediaViewerWidget::clearColor() const {
  return clear_color_;
}

void MediaViewerWidget::setPointInspectorEnabled(bool enabled) {
  if (point_inspector_enabled_.load(std::memory_order_relaxed) == enabled) {
    return;
  }
  point_inspector_enabled_.store(enabled, std::memory_order_relaxed);
  if (!enabled) {
    hidePointInspector();
    return;
  }
  refreshPointInspector();
}

bool MediaViewerWidget::pointInspectorEnabled() const noexcept {
  return point_inspector_enabled_.load(std::memory_order_relaxed);
}

void MediaViewerWidget::resetView() {
  zoom_ = 1.0f;
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  update();
}

void MediaViewerWidget::setMediaSource(MediaSource* source) {
  std::lock_guard lock(frame_mutex_);
  media_source_ = source;
  inspector_frame_ = {};
  point_inspector_active_.store(false, std::memory_order_relaxed);
  hidePointInspector();
  last_overlays_.clear();
  overlays_dirty_ = true;
  // Drop the previous source's pixel-layer stack so its segmentation/depth
  // overlays don't keep compositing over the new source. The GPU textures are
  // reconciled when the next frame arrives; until then pixel_layers_active_ is
  // false, so they stay inert.
  pending_pixel_layers_.clear();
  has_pending_pixel_layers_ = false;
  pixel_layers_active_ = false;
  // Drop glyph textures keyed to the previous source's labels; new source
  // likely brings a different label set, and the cache currently has no LRU.
  clearTextCache();
}

namespace {

inline void pushVertex(std::vector<float>& out, double x, double y, const ColorRGBA& c) {
  out.push_back(static_cast<float>(x));
  out.push_back(static_cast<float>(y));
  out.push_back(static_cast<float>(c.r) / 255.0f);
  out.push_back(static_cast<float>(c.g) / 255.0f);
  out.push_back(static_cast<float>(c.b) / 255.0f);
  out.push_back(static_cast<float>(c.a) / 255.0f);
}

inline void appendSegment(std::vector<float>& out, const Point2& a, const Point2& b, const ColorRGBA& c) {
  pushVertex(out, a.x, a.y, c);
  pushVertex(out, b.x, b.y, c);
}

inline void appendSegment(
    std::vector<float>& out, const Point2& a, const Point2& b, const ColorRGBA& ca, const ColorRGBA& cb) {
  pushVertex(out, a.x, a.y, ca);
  pushVertex(out, b.x, b.y, cb);
}

inline ColorRGBA vertexColor(const PointsAnnotation& pa, size_t i) {
  if (pa.colors.size() == pa.points.size()) {
    return pa.colors[i];
  }
  return pa.color;
}

// 1 px native pipeline (Lines topology). Per-vertex colors honoured when
// pa.colors.size() == pa.points.size(); otherwise pa.color is splatted.
void expandToLineList(const PointsAnnotation& pa, std::vector<float>& out) {
  const auto& pts = pa.points;
  if (pts.size() < 2) {
    return;
  }
  switch (pa.topology) {
    case AnnotationTopology::kLineLoop:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1));
      }
      appendSegment(out, pts.back(), pts.front(), vertexColor(pa, pts.size() - 1), vertexColor(pa, 0));
      break;
    case AnnotationTopology::kLineStrip:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1));
      }
      break;
    case AnnotationTopology::kLineList:
      for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        appendSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1));
      }
      break;
    case AnnotationTopology::kPoints:
      // Handled by expandKPointsToQuads (Triangles pipeline).
      break;
  }
}

void expandKPointsToQuads(const PointsAnnotation& pa, std::vector<float>& out) {
  if (pa.topology != AnnotationTopology::kPoints || pa.points.empty()) {
    return;
  }
  const double h = pa.thickness * 0.5;
  for (size_t i = 0; i < pa.points.size(); ++i) {
    const auto& p = pa.points[i];
    const ColorRGBA c = vertexColor(pa, i);
    pushVertex(out, p.x - h, p.y - h, c);
    pushVertex(out, p.x + h, p.y - h, c);
    pushVertex(out, p.x + h, p.y + h, c);
    pushVertex(out, p.x - h, p.y - h, c);
    pushVertex(out, p.x + h, p.y + h, c);
    pushVertex(out, p.x - h, p.y + h, c);
  }
}

inline int circleSegments(double radius) {
  return radius < 10.0 ? 32 : 64;
}

inline std::vector<Point2> circlePerimeter(const CircleAnnotation& c, int n) {
  std::vector<Point2> out;
  out.reserve(static_cast<size_t>(n));
  const double step = 2.0 * 3.14159265358979323846 / static_cast<double>(n);
  for (int i = 0; i < n; ++i) {
    const double a = step * static_cast<double>(i);
    out.push_back({c.center.x + c.radius * std::cos(a), c.center.y + c.radius * std::sin(a)});
  }
  return out;
}

// Each segment becomes 2 triangles forming a rectangle perpendicular to ab.
// No miter joins — adjacent segments butt-join with a possible visible gap at
// sharp angles, acceptable for bboxes and gentle polylines.
inline void appendThickSegment(
    std::vector<float>& out, const Point2& a, const Point2& b, const ColorRGBA& ca, const ColorRGBA& cb,
    double thickness) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-6) {
    return;
  }
  const double half = thickness * 0.5;
  // Perpendicular unit vector × half thickness.
  const double nx = -dy / len * half;
  const double ny = dx / len * half;
  const Point2 a1{a.x - nx, a.y - ny};
  const Point2 a2{a.x + nx, a.y + ny};
  const Point2 b1{b.x - nx, b.y - ny};
  const Point2 b2{b.x + nx, b.y + ny};
  pushVertex(out, a1.x, a1.y, ca);
  pushVertex(out, a2.x, a2.y, ca);
  pushVertex(out, b2.x, b2.y, cb);
  pushVertex(out, a1.x, a1.y, ca);
  pushVertex(out, b2.x, b2.y, cb);
  pushVertex(out, b1.x, b1.y, cb);
}

void expandToThickList(const PointsAnnotation& pa, std::vector<float>& out) {
  const auto& pts = pa.points;
  if (pts.size() < 2) {
    return;
  }
  const double t = pa.thickness;
  switch (pa.topology) {
    case AnnotationTopology::kLineLoop:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), t);
      }
      appendThickSegment(out, pts.back(), pts.front(), vertexColor(pa, pts.size() - 1), vertexColor(pa, 0), t);
      break;
    case AnnotationTopology::kLineStrip:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), t);
      }
      break;
    case AnnotationTopology::kLineList:
      for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), t);
      }
      break;
    case AnnotationTopology::kPoints:
      break;
  }
}

void expandCircleOutlineToThick(const CircleAnnotation& c, const std::vector<Point2>& perim, std::vector<float>& out) {
  if (c.color.a == 0 || c.radius <= 0.0) {
    return;
  }
  for (size_t i = 0; i < perim.size(); ++i) {
    appendThickSegment(out, perim[i], perim[(i + 1) % perim.size()], c.color, c.color, c.thickness);
  }
}

// Triangle fan from points[0] — convex-only. Non-convex polygons render with
// self-overlapping triangles (acceptable for bbox/triangle/regular shapes).
void expandLoopFillToTriangles(const PointsAnnotation& pa, std::vector<float>& out) {
  if (pa.topology != AnnotationTopology::kLineLoop || pa.fill_color.a == 0 || pa.points.size() < 3) {
    return;
  }
  const Point2& p0 = pa.points[0];
  for (size_t i = 1; i + 1 < pa.points.size(); ++i) {
    pushVertex(out, p0.x, p0.y, pa.fill_color);
    pushVertex(out, pa.points[i].x, pa.points[i].y, pa.fill_color);
    pushVertex(out, pa.points[i + 1].x, pa.points[i + 1].y, pa.fill_color);
  }
}

void expandCircleOutlineToLineList(
    const CircleAnnotation& c, const std::vector<Point2>& perim, std::vector<float>& out) {
  if (c.color.a == 0 || c.radius <= 0.0) {
    return;
  }
  for (size_t i = 0; i < perim.size(); ++i) {
    const Point2& a = perim[i];
    const Point2& b = perim[(i + 1) % perim.size()];
    appendSegment(out, a, b, c.color);
  }
}

void expandCircleFillToTriangleFan(
    const CircleAnnotation& c, const std::vector<Point2>& perim, std::vector<float>& out) {
  if (c.fill_color.a == 0 || c.radius <= 0.0) {
    return;
  }
  for (size_t i = 0; i < perim.size(); ++i) {
    const Point2& a = perim[i];
    const Point2& b = perim[(i + 1) % perim.size()];
    pushVertex(out, c.center.x, c.center.y, c.fill_color);
    pushVertex(out, a.x, a.y, c.fill_color);
    pushVertex(out, b.x, b.y, c.fill_color);
  }
}

QRhiScissor imageScissor(const QMatrix4x4& view, const QSize& output_size) {
  if (output_size.width() <= 0 || output_size.height() <= 0) {
    return QRhiScissor(0, 0, 0, 0);
  }

  float min_x = 1.0e9f;
  float min_y = 1.0e9f;
  float max_x = -1.0e9f;
  float max_y = -1.0e9f;
  const QVector4D corners[] = {
      {-1.0f, -1.0f, 0.0f, 1.0f},
      {1.0f, -1.0f, 0.0f, 1.0f},
      {-1.0f, 1.0f, 0.0f, 1.0f},
      {1.0f, 1.0f, 0.0f, 1.0f},
  };

  for (const auto& corner : corners) {
    QVector4D point = view * corner;
    if (point.w() != 0.0f) {
      point /= point.w();
    }
    min_x = std::min(min_x, point.x());
    min_y = std::min(min_y, point.y());
    max_x = std::max(max_x, point.x());
    max_y = std::max(max_y, point.y());
  }

  min_x = std::clamp(min_x, -1.0f, 1.0f);
  min_y = std::clamp(min_y, -1.0f, 1.0f);
  max_x = std::clamp(max_x, -1.0f, 1.0f);
  max_y = std::clamp(max_y, -1.0f, 1.0f);
  if (max_x <= min_x || max_y <= min_y) {
    return QRhiScissor(0, 0, 0, 0);
  }

  const auto width = static_cast<float>(output_size.width());
  const auto height = static_cast<float>(output_size.height());
  const int x0 = std::max(0, static_cast<int>(std::floor((min_x * 0.5f + 0.5f) * width)));
  const int x1 = std::min(output_size.width(), static_cast<int>(std::ceil((max_x * 0.5f + 0.5f) * width)));
  const int y0 = std::max(0, static_cast<int>(std::floor((1.0f - max_y) * 0.5f * height)));
  const int y1 = std::min(output_size.height(), static_cast<int>(std::ceil((1.0f - min_y) * 0.5f * height)));
  return QRhiScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

}  // namespace

void MediaViewerWidget::setTimestamp(int64_t ts_ns) {
  if (media_source_ != nullptr) {
    media_source_->setTimestamp(ts_ns);
  }
}

bool MediaViewerWidget::hasRetainedUploadableFrameLocked() const {
  if (pending_decoded_.isNull()) {
    return false;
  }

  switch (pending_decoded_.format) {
    case PixelFormat::kRGB888:
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGR888:
    case PixelFormat::kBGRA8888:
    case PixelFormat::kMono8:
    case PixelFormat::kYUV420P:
      return true;
    case PixelFormat::kMono16:
    case PixelFormat::kNV12:
      return false;
  }
  return false;
}

void MediaViewerWidget::releaseResources() {
  hidePointInspector();
  delete pipeline_;
  pipeline_ = nullptr;
  delete composite_pipeline_;
  composite_pipeline_ = nullptr;
  delete srb_;
  srb_ = nullptr;
  delete tex_y_;
  tex_y_ = nullptr;
  delete tex_u_;
  tex_u_ = nullptr;
  delete tex_v_;
  tex_v_ = nullptr;
  delete sampler_;
  sampler_ = nullptr;
  delete uniform_buf_;
  uniform_buf_ = nullptr;
  clearPixelLayerTextures();
  delete marker_pipeline_;
  marker_pipeline_ = nullptr;
  delete marker_srb_;
  marker_srb_ = nullptr;
  delete marker_uniform_buf_;
  marker_uniform_buf_ = nullptr;
  delete marker_vbo_;
  marker_vbo_ = nullptr;
  marker_vbo_capacity_ = 0;
  delete points_pipeline_;
  points_pipeline_ = nullptr;
  delete points_srb_;
  points_srb_ = nullptr;
  delete points_vbo_;
  points_vbo_ = nullptr;
  points_vbo_capacity_ = 0;
  delete thick_pipeline_;
  thick_pipeline_ = nullptr;
  delete thick_srb_;
  thick_srb_ = nullptr;
  delete thick_vbo_;
  thick_vbo_ = nullptr;
  thick_vbo_capacity_ = 0;
  delete text_pipeline_;
  text_pipeline_ = nullptr;
  delete text_srb_;
  text_srb_ = nullptr;
  delete text_vbo_;
  text_vbo_ = nullptr;
  delete text_sampler_;
  text_sampler_ = nullptr;
  delete text_placeholder_tex_;
  text_placeholder_tex_ = nullptr;
  text_vbo_capacity_ = 0;
  clearTextCache();
  tex_width_ = 0;
  tex_height_ = 0;
  std::lock_guard lock(frame_mutex_);
  has_pending_pixel_layers_ = !pending_pixel_layers_.empty();
  has_pending_ = hasRetainedUploadableFrameLocked();
}

void MediaViewerWidget::clearTextCache() {
  for (auto& kv : text_cache_) {
    delete kv.second.srb;
    delete kv.second.tex;
  }
  text_cache_.clear();
  text_draw_items_.clear();
}

void MediaViewerWidget::clearPixelLayerTextures() {
  for (auto& layer : pixel_layer_textures_) {
    destroyTextureLayer(layer);
  }
  pixel_layer_textures_.clear();
}

void MediaViewerWidget::destroyTextureLayer(TextureLayerResources& layer) {
  delete layer.srb;
  delete layer.uniform_buf;
  delete layer.tex_y;
  delete layer.tex_u;
  delete layer.tex_v;
  layer = {};
}

bool MediaViewerWidget::ensureTextureLayer(TextureLayerResources& layer) {
  auto* r = rhi();
  if (r == nullptr || sampler_ == nullptr) {
    return false;
  }
  if (layer.uniform_buf == nullptr) {
    layer.uniform_buf = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBufSize);
    if (!layer.uniform_buf->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  if (layer.tex_y == nullptr) {
    layer.tex_y = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    layer.tex_u = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    layer.tex_v = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    if (!layer.tex_y->create() || !layer.tex_u->create() || !layer.tex_v->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  if (layer.srb == nullptr) {
    layer.srb = r->newShaderResourceBindings();
    layer.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, layer.uniform_buf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, layer.tex_y, sampler_),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, layer.tex_u, sampler_),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, layer.tex_v, sampler_),
    });
    if (!layer.srb->create()) {
      destroyTextureLayer(layer);
      return false;
    }
  }
  return true;
}

MediaViewerWidget::TexturePathFormat MediaViewerWidget::texturePathFor(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::kYUV420P:
      return TexturePathFormat::kYUV420P;
    case PixelFormat::kNV12:
      return TexturePathFormat::kNV12;  // reserved: see header — not produced yet
    case PixelFormat::kRGB888:
    case PixelFormat::kRGBA8888:
    case PixelFormat::kBGR888:
    case PixelFormat::kBGRA8888:
    case PixelFormat::kMono8:
    case PixelFormat::kMono16:
      return TexturePathFormat::kRGBA;
  }
  return TexturePathFormat::kRGBA;
}

bool MediaViewerWidget::uploadDecodedFrameToTexture(
    const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates) {
  if (frame.isNull() || frame.width <= 0 || frame.height <= 0 || frame.pixels == nullptr || updates == nullptr) {
    return false;
  }
  if (!ensureTextureLayer(layer)) {
    return false;
  }

  const int w = frame.width;
  const int h = frame.height;
  const uint8_t* src = frame.pixels->data();
  const size_t src_size = frame.pixels->size();

  if (texturePathFor(frame.format) == TexturePathFormat::kYUV420P) {
    const int uv_w = (w + 1) / 2;
    const int uv_h = (h + 1) / 2;
    const int y_size = w * h;
    const int uv_size = uv_w * uv_h;
    if (src_size < static_cast<size_t>(y_size + 2 * uv_size)) {
      return false;
    }

    if (w != layer.width || h != layer.height || layer.format != TexturePathFormat::kYUV420P) {
      layer.tex_y->destroy();
      layer.tex_y->setFormat(QRhiTexture::R8);
      layer.tex_y->setPixelSize(QSize(w, h));
      layer.tex_y->create();

      layer.tex_u->destroy();
      layer.tex_u->setFormat(QRhiTexture::R8);
      layer.tex_u->setPixelSize(QSize(uv_w, uv_h));
      layer.tex_u->create();

      layer.tex_v->destroy();
      layer.tex_v->setFormat(QRhiTexture::R8);
      layer.tex_v->setPixelSize(QSize(uv_w, uv_h));
      layer.tex_v->create();

      layer.srb->destroy();
      layer.srb->setBindings({
          QRhiShaderResourceBinding::uniformBuffer(
              0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, layer.uniform_buf),
          QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, layer.tex_y, sampler_),
          QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, layer.tex_u, sampler_),
          QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, layer.tex_v, sampler_),
      });
      layer.srb->create();

      layer.width = w;
      layer.height = h;
      layer.format = TexturePathFormat::kYUV420P;
    }

    QRhiTextureSubresourceUploadDescription y_desc(src, y_size);
    y_desc.setSourceSize(QSize(w, h));
    updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, y_desc}));

    QRhiTextureSubresourceUploadDescription u_desc(src + y_size, uv_size);
    u_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_u, QRhiTextureUploadDescription({0, 0, u_desc}));

    QRhiTextureSubresourceUploadDescription v_desc(src + y_size + uv_size, uv_size);
    v_desc.setSourceSize(QSize(uv_w, uv_h));
    updates->uploadTexture(layer.tex_v, QRhiTextureUploadDescription({0, 0, v_desc}));
    return true;
  }

  std::vector<uint8_t> rgba_buf;
  const uint8_t* rgba_data = nullptr;
  size_t rgba_size = 0;
  const bool is_bgr = (frame.format == PixelFormat::kBGR888 || frame.format == PixelFormat::kBGRA8888);

  if (frame.format == PixelFormat::kRGBA8888) {
    rgba_data = src;
    rgba_size = src_size;
  } else if (frame.format == PixelFormat::kBGRA8888) {
    rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);
    const int pixel_count = w * h;
    for (int i = 0; i < pixel_count; ++i) {
      rgba_buf[i * 4 + 0] = src[i * 4 + 2];
      rgba_buf[i * 4 + 1] = src[i * 4 + 1];
      rgba_buf[i * 4 + 2] = src[i * 4 + 0];
      rgba_buf[i * 4 + 3] = src[i * 4 + 3];
    }
    rgba_data = rgba_buf.data();
    rgba_size = rgba_buf.size();
  } else if (frame.format == PixelFormat::kRGB888 || frame.format == PixelFormat::kBGR888) {
    rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);
    const int pixel_count = w * h;
    for (int i = 0; i < pixel_count; ++i) {
      rgba_buf[i * 4 + 0] = src[i * 3 + (is_bgr ? 2 : 0)];
      rgba_buf[i * 4 + 1] = src[i * 3 + 1];
      rgba_buf[i * 4 + 2] = src[i * 3 + (is_bgr ? 0 : 2)];
      rgba_buf[i * 4 + 3] = 255;
    }
    rgba_data = rgba_buf.data();
    rgba_size = rgba_buf.size();
  } else if (frame.format == PixelFormat::kMono8) {
    rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4U);
    const int pixel_count = w * h;
    for (int i = 0; i < pixel_count; ++i) {
      const uint8_t g = src[i];
      rgba_buf[i * 4 + 0] = g;
      rgba_buf[i * 4 + 1] = g;
      rgba_buf[i * 4 + 2] = g;
      rgba_buf[i * 4 + 3] = 255;
    }
    rgba_data = rgba_buf.data();
    rgba_size = rgba_buf.size();
  }

  if (rgba_data == nullptr) {
    return false;
  }

  if (w != layer.width || h != layer.height || layer.format != TexturePathFormat::kRGBA) {
    layer.tex_y->destroy();
    layer.tex_y->setFormat(QRhiTexture::RGBA8);
    layer.tex_y->setPixelSize(QSize(w, h));
    layer.tex_y->create();

    layer.srb->destroy();
    layer.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, layer.uniform_buf),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, layer.tex_y, sampler_),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, layer.tex_u, sampler_),
        QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, layer.tex_v, sampler_),
    });
    layer.srb->create();

    layer.width = w;
    layer.height = h;
    layer.format = TexturePathFormat::kRGBA;
  }

  QRhiTextureSubresourceUploadDescription sub_desc(rgba_data, static_cast<quint32>(rgba_size));
  sub_desc.setSourceSize(QSize(w, h));
  updates->uploadTexture(layer.tex_y, QRhiTextureUploadDescription({0, 0, sub_desc}));
  return true;
}

void MediaViewerWidget::updateTextureLayerUniform(
    TextureLayerResources& layer, const QMatrix4x4& view, QRhiResourceUpdateBatch* updates) const {
  if (layer.uniform_buf == nullptr || updates == nullptr) {
    return;
  }
  updates->updateDynamicBuffer(layer.uniform_buf, 0, 64, view.constData());
  updates->updateDynamicBuffer(layer.uniform_buf, 64, 64, kBT709);
  const int32_t fmt = static_cast<int32_t>(layer.format);
  updates->updateDynamicBuffer(layer.uniform_buf, 128, 4, &fmt);
  updates->updateDynamicBuffer(layer.uniform_buf, 132, 4, &layer.opacity);
}

void MediaViewerWidget::initialize(QRhiCommandBuffer* /*cb*/) {
  auto* r = rhi();
  if (r == nullptr) {
    return;
  }

  if (rhi_cached_ != r) {
    releaseResources();
    rhi_cached_ = r;
  }

  if (pipeline_ != nullptr) {
    return;
  }

  // Use YUV→RGB shader (handles both YUV and RGBA passthrough)
  auto vert = loadShader(":/shaders/yuv_to_rgb.vert.qsb");
  auto frag = loadShader(":/shaders/yuv_to_rgb.frag.qsb");
  if (!vert.isValid() || !frag.isValid()) {
    qWarning("MediaViewerWidget: failed to load yuv_to_rgb shaders");
    return;
  }

  // Uniform buffer: viewTransform (64) + colorMatrix (64) + pixelFormat (4) + padding (12) = 144
  uniform_buf_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBufSize);
  uniform_buf_->create();

  sampler_ = r->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
  sampler_->create();

  // Create placeholder textures (1x1) — resized on first frame
  tex_y_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));
  tex_y_->create();
  tex_u_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));
  tex_u_->create();
  tex_v_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));
  tex_v_->create();

  srb_ = r->newShaderResourceBindings();
  srb_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniform_buf_),
      QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex_y_, sampler_),
      QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, tex_u_, sampler_),
      QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, tex_v_, sampler_),
  });
  srb_->create();

  pipeline_ = r->newGraphicsPipeline();
  pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  pipeline_->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});

  QRhiVertexInputLayout input_layout;
  pipeline_->setVertexInputLayout(input_layout);
  pipeline_->setShaderResourceBindings(srb_);
  pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!pipeline_->create()) {
    qWarning("MediaViewerWidget: failed to create graphics pipeline");
    pipeline_ = nullptr;
    return;
  }

  composite_pipeline_ = r->newGraphicsPipeline();
  composite_pipeline_->setFlags(QRhiGraphicsPipeline::UsesScissor);
  composite_pipeline_->setShaderStages(
      {QRhiShaderStage(QRhiShaderStage::Vertex, vert), QRhiShaderStage(QRhiShaderStage::Fragment, frag)});
  composite_pipeline_->setVertexInputLayout(input_layout);
  composite_pipeline_->setShaderResourceBindings(srb_);
  composite_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  QRhiGraphicsPipeline::TargetBlend image_blend;
  image_blend.enable = true;
  image_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  image_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  image_blend.srcAlpha = QRhiGraphicsPipeline::One;
  image_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  composite_pipeline_->setTargetBlends({image_blend});
  if (!composite_pipeline_->create()) {
    qWarning("MediaViewerWidget: failed to create composite graphics pipeline");
    delete composite_pipeline_;
    composite_pipeline_ = nullptr;
  }

  // ----- Marker / overlay pipeline -----
  auto marker_vert = loadShader(":/shaders/scene_lines.vert.qsb");
  auto marker_frag = loadShader(":/shaders/scene_lines.frag.qsb");
  if (marker_vert.isValid() && marker_frag.isValid()) {
    marker_uniform_buf_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMarkerUniformBufSize);
    marker_uniform_buf_->create();

    // Initial VBO capacity ~64KB (≈ 2700 vertices = 340 bboxes' worth).
    marker_vbo_capacity_ = 64 * 1024;
    marker_vbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, static_cast<int>(marker_vbo_capacity_));
    marker_vbo_->create();

    marker_srb_ = r->newShaderResourceBindings();
    marker_srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
    });
    marker_srb_->create();

    QRhiVertexInputLayout marker_layout;
    QRhiVertexInputBinding binding(24);  // stride: 8 (vec2 pos) + 16 (vec4 color)
    marker_layout.setBindings({binding});
    QRhiVertexInputAttribute pos_attr(0, 0, QRhiVertexInputAttribute::Float2, 0);
    QRhiVertexInputAttribute color_attr(0, 1, QRhiVertexInputAttribute::Float4, 8);
    marker_layout.setAttributes({pos_attr, color_attr});

    marker_pipeline_ = r->newGraphicsPipeline();
    marker_pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, marker_vert),
         QRhiShaderStage(QRhiShaderStage::Fragment, marker_frag)});
    marker_pipeline_->setTopology(QRhiGraphicsPipeline::Lines);
    marker_pipeline_->setVertexInputLayout(marker_layout);
    marker_pipeline_->setShaderResourceBindings(marker_srb_);
    marker_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    marker_pipeline_->setTargetBlends({blend});

    if (!marker_pipeline_->create()) {
      qWarning("MediaViewerWidget: failed to create marker pipeline");
      delete marker_pipeline_;
      marker_pipeline_ = nullptr;
    }
  } else {
    qWarning("MediaViewerWidget: scene_lines shaders not loaded; markers disabled");
  }

  // ----- kPoints quad pipeline -----
  // Solid-fill triangles, shares marker_uniform_buf_ but has its own SRB and VBO.
  auto quads_vert = loadShader(":/shaders/scene_quads.vert.qsb");
  auto quads_frag = loadShader(":/shaders/scene_quads.frag.qsb");
  if (quads_vert.isValid() && quads_frag.isValid() && marker_uniform_buf_ != nullptr) {
    points_vbo_capacity_ = 64 * 1024;
    points_vbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, static_cast<int>(points_vbo_capacity_));
    points_vbo_->create();

    points_srb_ = r->newShaderResourceBindings();
    points_srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
    });
    points_srb_->create();

    QRhiVertexInputLayout points_layout;
    QRhiVertexInputBinding p_binding(24);  // same stride as marker pipeline
    points_layout.setBindings({p_binding});
    QRhiVertexInputAttribute p_pos(0, 0, QRhiVertexInputAttribute::Float2, 0);
    QRhiVertexInputAttribute p_color(0, 1, QRhiVertexInputAttribute::Float4, 8);
    points_layout.setAttributes({p_pos, p_color});

    points_pipeline_ = r->newGraphicsPipeline();
    points_pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, quads_vert), QRhiShaderStage(QRhiShaderStage::Fragment, quads_frag)});
    points_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    points_pipeline_->setVertexInputLayout(points_layout);
    points_pipeline_->setShaderResourceBindings(points_srb_);
    points_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

    QRhiGraphicsPipeline::TargetBlend p_blend;
    p_blend.enable = true;
    p_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    p_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    p_blend.srcAlpha = QRhiGraphicsPipeline::One;
    p_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    points_pipeline_->setTargetBlends({p_blend});

    if (!points_pipeline_->create()) {
      qWarning("MediaViewerWidget: failed to create points pipeline");
      delete points_pipeline_;
      points_pipeline_ = nullptr;
    }
  } else if (!quads_vert.isValid() || !quads_frag.isValid()) {
    qWarning("MediaViewerWidget: scene_quads shaders not loaded; kPoints disabled");
  }

  // ----- Thick lines pipeline (Triangles, reuses scene_lines shaders) -----
  // Same vertex layout as marker (vec2 pos + vec4 color, stride 24). The
  // rectangle expansion is CPU-side in expandToThickList — the shaders are
  // unchanged, only the topology differs.
  if (marker_vert.isValid() && marker_frag.isValid() && marker_uniform_buf_ != nullptr) {
    thick_vbo_capacity_ = 64 * 1024;
    thick_vbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, static_cast<int>(thick_vbo_capacity_));
    thick_vbo_->create();

    thick_srb_ = r->newShaderResourceBindings();
    thick_srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
    });
    thick_srb_->create();

    QRhiVertexInputLayout thick_layout;
    QRhiVertexInputBinding t_binding(24);
    thick_layout.setBindings({t_binding});
    QRhiVertexInputAttribute t_pos(0, 0, QRhiVertexInputAttribute::Float2, 0);
    QRhiVertexInputAttribute t_color(0, 1, QRhiVertexInputAttribute::Float4, 8);
    thick_layout.setAttributes({t_pos, t_color});

    thick_pipeline_ = r->newGraphicsPipeline();
    thick_pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, marker_vert),
         QRhiShaderStage(QRhiShaderStage::Fragment, marker_frag)});
    thick_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    thick_pipeline_->setVertexInputLayout(thick_layout);
    thick_pipeline_->setShaderResourceBindings(thick_srb_);
    thick_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

    QRhiGraphicsPipeline::TargetBlend t_blend;
    t_blend.enable = true;
    t_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    t_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    t_blend.srcAlpha = QRhiGraphicsPipeline::One;
    t_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    thick_pipeline_->setTargetBlends({t_blend});

    if (!thick_pipeline_->create()) {
      qWarning("MediaViewerWidget: failed to create thick pipeline");
      delete thick_pipeline_;
      thick_pipeline_ = nullptr;
    }
  }

  // ----- Text pipeline (textured quads, R8 alpha mask + tint color) -----
  auto text_vert = loadShader(":/shaders/scene_text.vert.qsb");
  auto text_frag = loadShader(":/shaders/scene_text.frag.qsb");
  if (text_vert.isValid() && text_frag.isValid() && marker_uniform_buf_ != nullptr) {
    text_vbo_capacity_ = 32 * 1024;
    text_vbo_ = r->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, static_cast<int>(text_vbo_capacity_));
    text_vbo_->create();

    text_sampler_ = r->newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);
    text_sampler_->create();

    // Placeholder 1x1 alpha texture used only as the pipeline's layout-compat SRB.
    // Real per-draw SRBs are stored in TextEntry inside text_cache_.
    text_placeholder_tex_ = r->newTexture(QRhiTexture::R8, QSize(1, 1));
    text_placeholder_tex_->create();

    text_srb_ = r->newShaderResourceBindings();
    text_srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, text_placeholder_tex_, text_sampler_),
    });
    text_srb_->create();

    QRhiVertexInputLayout text_layout;
    QRhiVertexInputBinding text_binding(32);  // pos vec2 + uv vec2 + color vec4 = 32 bytes
    text_layout.setBindings({text_binding});
    QRhiVertexInputAttribute text_pos(0, 0, QRhiVertexInputAttribute::Float2, 0);
    QRhiVertexInputAttribute text_uv(0, 1, QRhiVertexInputAttribute::Float2, 8);
    QRhiVertexInputAttribute text_color(0, 2, QRhiVertexInputAttribute::Float4, 16);
    text_layout.setAttributes({text_pos, text_uv, text_color});

    text_pipeline_ = r->newGraphicsPipeline();
    text_pipeline_->setShaderStages(
        {QRhiShaderStage(QRhiShaderStage::Vertex, text_vert), QRhiShaderStage(QRhiShaderStage::Fragment, text_frag)});
    text_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
    text_pipeline_->setVertexInputLayout(text_layout);
    text_pipeline_->setShaderResourceBindings(text_srb_);
    text_pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

    QRhiGraphicsPipeline::TargetBlend tx_blend;
    tx_blend.enable = true;
    tx_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    tx_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    tx_blend.srcAlpha = QRhiGraphicsPipeline::One;
    tx_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    text_pipeline_->setTargetBlends({tx_blend});

    if (!text_pipeline_->create()) {
      qWarning("MediaViewerWidget: failed to create text pipeline");
      delete text_pipeline_;
      text_pipeline_ = nullptr;
    }
  } else if (!text_vert.isValid() || !text_frag.isValid()) {
    qWarning("MediaViewerWidget: scene_text shaders not loaded; text disabled");
  }

  if (has_pending_ || has_pending_pixel_layers_) {
    update();
  }
}

void MediaViewerWidget::render(QRhiCommandBuffer* cb) {
  if (pipeline_ == nullptr) {
    return;
  }
  auto* r = rhi();
  if (r == nullptr) {
    return;
  }

  auto* rt = renderTarget();
  const QSize output_size = rt->pixelSize();
  QRhiResourceUpdateBatch* updates = r->nextResourceUpdateBatch();
  bool inspector_frame_changed = false;

  {
    std::lock_guard lock(frame_mutex_);

    // Poll MediaSource if attached. MediaFrame may carry both a pixel base
    // and vector overlays; capture each independently — a frame can update
    // either layer in isolation.
    if (media_source_ != nullptr) {
      auto frame = media_source_->takeFrame();
      if (frame.has_value()) {
        if (!frame->pixel_layers.empty()) {
          pending_pixel_layers_ = std::move(frame->pixel_layers);
          has_pending_pixel_layers_ = true;
          pixel_layers_active_ = true;
          has_pending_ = false;
          pending_decoded_ = {};
          for (const auto& layer : pending_pixel_layers_) {
            if (!layer.frame.isNull()) {
              inspector_frame_ = layer.frame;
              inspector_frame_changed = true;
              break;
            }
          }
        } else if (frame->base.has_value() && !frame->base->isNull()) {
          pending_decoded_ = std::move(*frame->base);
          pending_is_yuv_ = texturePathFor(pending_decoded_.format) == TexturePathFormat::kYUV420P;
          pending_pixel_layers_.clear();
          has_pending_pixel_layers_ = false;
          pixel_layers_active_ = false;
          inspector_frame_ = pending_decoded_;
          inspector_frame_changed = true;
          has_pending_ = true;
        }
        if (!frame->overlays.empty()) {
          last_overlays_ = std::move(frame->overlays);
          overlays_dirty_ = true;
        }
      }
    }

    if (has_pending_pixel_layers_) {
      if (pending_pixel_layers_.size() < pixel_layer_textures_.size()) {
        for (size_t i = pending_pixel_layers_.size(); i < pixel_layer_textures_.size(); ++i) {
          destroyTextureLayer(pixel_layer_textures_[i]);
        }
      }
      pixel_layer_textures_.resize(pending_pixel_layers_.size());

      bool set_frame_size = false;
      for (size_t i = 0; i < pending_pixel_layers_.size(); ++i) {
        auto& texture = pixel_layer_textures_[i];
        texture.opacity = std::clamp(pending_pixel_layers_[i].opacity, 0.0f, 1.0f);
        if (uploadDecodedFrameToTexture(pending_pixel_layers_[i].frame, texture, updates) && !set_frame_size) {
          tex_width_ = texture.width;
          tex_height_ = texture.height;
          frame_aspect_ = static_cast<float>(texture.width) / static_cast<float>(texture.height);
          set_frame_size = true;
        }
      }
      has_pending_pixel_layers_ = false;
    }

    if (has_pending_) {
      if (pending_is_yuv_ && !pending_decoded_.isNull()) {
        // YUV420P path: upload 3 planes to separate R8 textures
        int w = pending_decoded_.width;
        int h = pending_decoded_.height;
        int uv_w = (w + 1) / 2;
        int uv_h = (h + 1) / 2;
        const uint8_t* pixel_data = pending_decoded_.pixels->data();
        int y_size = w * h;
        int uv_size = uv_w * uv_h;

        if (w != tex_width_ || h != tex_height_ || current_pixel_format_ != TexturePathFormat::kYUV420P) {
          // Recreate textures at correct size
          tex_y_->destroy();
          tex_y_->setFormat(QRhiTexture::R8);
          tex_y_->setPixelSize(QSize(w, h));
          tex_y_->create();

          tex_u_->destroy();
          tex_u_->setFormat(QRhiTexture::R8);
          tex_u_->setPixelSize(QSize(uv_w, uv_h));
          tex_u_->create();

          tex_v_->destroy();
          tex_v_->setFormat(QRhiTexture::R8);
          tex_v_->setPixelSize(QSize(uv_w, uv_h));
          tex_v_->create();

          // Rebuild SRB with new textures
          srb_->destroy();
          srb_->setBindings({
              QRhiShaderResourceBinding::uniformBuffer(
                  0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniform_buf_),
              QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex_y_, sampler_),
              QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, tex_u_, sampler_),
              QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage, tex_v_, sampler_),
          });
          srb_->create();

          tex_width_ = w;
          tex_height_ = h;
          current_pixel_format_ = TexturePathFormat::kYUV420P;
        }

        QRhiTextureSubresourceUploadDescription y_desc(pixel_data, y_size);
        y_desc.setSourceSize(QSize(w, h));
        updates->uploadTexture(tex_y_, QRhiTextureUploadDescription({0, 0, y_desc}));

        QRhiTextureSubresourceUploadDescription u_desc(pixel_data + y_size, uv_size);
        u_desc.setSourceSize(QSize(uv_w, uv_h));
        updates->uploadTexture(tex_u_, QRhiTextureUploadDescription({0, 0, u_desc}));

        QRhiTextureSubresourceUploadDescription v_desc(pixel_data + y_size + uv_size, uv_size);
        v_desc.setSourceSize(QSize(uv_w, uv_h));
        updates->uploadTexture(tex_v_, QRhiTextureUploadDescription({0, 0, v_desc}));

        frame_aspect_ = static_cast<float>(w) / static_cast<float>(h);

      } else if (!pending_is_yuv_ && !pending_decoded_.isNull()) {
        // RGB/RGBA DecodedFrame path: convert to RGBA8 and upload as single texture.
        int w = pending_decoded_.width;
        int h = pending_decoded_.height;
        const uint8_t* src = pending_decoded_.pixels->data();
        size_t src_size = pending_decoded_.pixels->size();

        std::vector<uint8_t> rgba_buf;
        const uint8_t* rgba_data = nullptr;
        size_t rgba_size = 0;

        bool is_bgr =
            (pending_decoded_.format == PixelFormat::kBGR888 || pending_decoded_.format == PixelFormat::kBGRA8888);

        if (pending_decoded_.format == PixelFormat::kRGBA8888) {
          rgba_data = src;
          rgba_size = src_size;
        } else if (pending_decoded_.format == PixelFormat::kBGRA8888) {
          // BGRA→RGBA: swap R and B channels
          rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
          int pixel_count = w * h;
          for (int i = 0; i < pixel_count; ++i) {
            rgba_buf[i * 4 + 0] = src[i * 4 + 2];  // R ← B
            rgba_buf[i * 4 + 1] = src[i * 4 + 1];  // G
            rgba_buf[i * 4 + 2] = src[i * 4 + 0];  // B ← R
            rgba_buf[i * 4 + 3] = src[i * 4 + 3];  // A
          }
          rgba_data = rgba_buf.data();
          rgba_size = rgba_buf.size();
        } else if (pending_decoded_.format == PixelFormat::kRGB888 || pending_decoded_.format == PixelFormat::kBGR888) {
          // RGB/BGR→RGBA: insert alpha=255, swap R/B if BGR
          rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
          int pixel_count = w * h;
          for (int i = 0; i < pixel_count; ++i) {
            rgba_buf[i * 4 + 0] = src[i * 3 + (is_bgr ? 2 : 0)];
            rgba_buf[i * 4 + 1] = src[i * 3 + 1];
            rgba_buf[i * 4 + 2] = src[i * 3 + (is_bgr ? 0 : 2)];
            rgba_buf[i * 4 + 3] = 255;
          }
          rgba_data = rgba_buf.data();
          rgba_size = rgba_buf.size();
        } else if (pending_decoded_.format == PixelFormat::kMono8) {
          // Mono8→RGBA: replicate luma to RGB, alpha=255
          rgba_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
          int pixel_count = w * h;
          for (int i = 0; i < pixel_count; ++i) {
            uint8_t g = src[i];
            rgba_buf[i * 4 + 0] = g;
            rgba_buf[i * 4 + 1] = g;
            rgba_buf[i * 4 + 2] = g;
            rgba_buf[i * 4 + 3] = 255;
          }
          rgba_data = rgba_buf.data();
          rgba_size = rgba_buf.size();
        }

        if (rgba_data != nullptr) {
          if (w != tex_width_ || h != tex_height_ || current_pixel_format_ != TexturePathFormat::kRGBA) {
            tex_y_->destroy();
            tex_y_->setFormat(QRhiTexture::RGBA8);
            tex_y_->setPixelSize(QSize(w, h));
            tex_y_->create();

            srb_->destroy();
            srb_->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniform_buf_),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, tex_y_, sampler_),
                QRhiShaderResourceBinding::sampledTexture(
                    2, QRhiShaderResourceBinding::FragmentStage, tex_u_, sampler_),
                QRhiShaderResourceBinding::sampledTexture(
                    3, QRhiShaderResourceBinding::FragmentStage, tex_v_, sampler_),
            });
            srb_->create();

            tex_width_ = w;
            tex_height_ = h;
            current_pixel_format_ = TexturePathFormat::kRGBA;
          }

          QRhiTextureSubresourceUploadDescription sub_desc(rgba_data, static_cast<quint32>(rgba_size));
          sub_desc.setSourceSize(QSize(w, h));
          updates->uploadTexture(tex_y_, QRhiTextureUploadDescription({0, 0, sub_desc}));
          frame_aspect_ = static_cast<float>(w) / static_cast<float>(h);
        }
      }

      has_pending_ = false;
    }
  }

  if (inspector_frame_changed) {
    schedulePointInspectorRefresh();
  }

  // Update uniforms
  QMatrix4x4 view = buildViewTransform(output_size);
  updates->updateDynamicBuffer(uniform_buf_, 0, 64, view.constData());
  updates->updateDynamicBuffer(uniform_buf_, 64, 64, kBT709);
  int32_t fmt = static_cast<int32_t>(current_pixel_format_);
  updates->updateDynamicBuffer(uniform_buf_, 128, 4, &fmt);
  const float opacity = 1.0f;
  updates->updateDynamicBuffer(uniform_buf_, 132, 4, &opacity);
  if (pixel_layers_active_) {
    for (auto& layer : pixel_layer_textures_) {
      updateTextureLayerUniform(layer, view, updates);
    }
  }

  // ----- Marker pipeline: rebuild VBO + update uniforms -----
  size_t marker_vertex_count = 0;
  if (marker_pipeline_ != nullptr) {
    if (overlays_dirty_) {
      marker_vertex_data_.clear();
      thick_vertex_data_.clear();
      points_vertex_data_.clear();
      size_t total_points = 0;
      for (const auto& sf : last_overlays_) {
        for (const auto& ia : sf.annotations) {
          for (const auto& pa : ia.points) {
            total_points += pa.points.size();
          }
        }
      }
      marker_vertex_data_.reserve(total_points * 12);
      thick_vertex_data_.reserve(total_points * 36);
      points_vertex_data_.reserve(total_points * 36);
      for (const auto& sf : last_overlays_) {
        for (const auto& ia : sf.annotations) {
          for (const auto& pa : ia.points) {
            if (pa.thickness > 1.5) {
              expandToThickList(pa, thick_vertex_data_);
            } else {
              expandToLineList(pa, marker_vertex_data_);
            }
            expandLoopFillToTriangles(pa, points_vertex_data_);
            expandKPointsToQuads(pa, points_vertex_data_);
          }
          for (const auto& ca : ia.circles) {
            const auto perim = circlePerimeter(ca, circleSegments(ca.radius));
            if (ca.thickness > 1.5) {
              expandCircleOutlineToThick(ca, perim, thick_vertex_data_);
            } else {
              expandCircleOutlineToLineList(ca, perim, marker_vertex_data_);
            }
            if (points_pipeline_ != nullptr) {
              expandCircleFillToTriangleFan(ca, perim, points_vertex_data_);
            }
          }
        }
      }
      const size_t needed = marker_vertex_data_.size() * sizeof(float);
      if (needed > marker_vbo_capacity_) {
        marker_vbo_->destroy();
        marker_vbo_capacity_ = std::max(needed * 2, marker_vbo_capacity_);
        marker_vbo_->setSize(static_cast<int>(marker_vbo_capacity_));
        marker_vbo_->create();
      }
      if (needed > 0) {
        updates->updateDynamicBuffer(marker_vbo_, 0, static_cast<int>(needed), marker_vertex_data_.data());
      }

      // ----- Thick lines VBO upload (same dirty cycle) -----
      if (thick_pipeline_ != nullptr) {
        const size_t t_needed = thick_vertex_data_.size() * sizeof(float);
        if (t_needed > thick_vbo_capacity_) {
          thick_vbo_->destroy();
          thick_vbo_capacity_ = std::max(t_needed * 2, thick_vbo_capacity_);
          thick_vbo_->setSize(static_cast<int>(thick_vbo_capacity_));
          thick_vbo_->create();
        }
        if (t_needed > 0) {
          updates->updateDynamicBuffer(thick_vbo_, 0, static_cast<int>(t_needed), thick_vertex_data_.data());
        }
      }

      // ----- Points (Triangles) VBO upload — same data filled in the loop above -----
      if (points_pipeline_ != nullptr) {
        const size_t p_needed = points_vertex_data_.size() * sizeof(float);
        if (p_needed > points_vbo_capacity_) {
          points_vbo_->destroy();
          points_vbo_capacity_ = std::max(p_needed * 2, points_vbo_capacity_);
          points_vbo_->setSize(static_cast<int>(points_vbo_capacity_));
          points_vbo_->create();
        }
        if (p_needed > 0) {
          updates->updateDynamicBuffer(points_vbo_, 0, static_cast<int>(p_needed), points_vertex_data_.data());
        }
      }

      // ----- Text rebuild (textured quads) -----
      if (text_pipeline_ != nullptr) {
        text_vertex_data_.clear();
        text_draw_items_.clear();
        constexpr size_t kFloatsPerTextQuad = 6 * 8;  // 6 verts × (pos2+uv2+color4) = 48 floats
        size_t total_texts = 0;
        for (const auto& sf : last_overlays_) {
          for (const auto& ia : sf.annotations) {
            total_texts += ia.texts.size();
          }
        }
        text_vertex_data_.reserve(total_texts * kFloatsPerTextQuad);
        for (const auto& sf : last_overlays_) {
          for (const auto& ia : sf.annotations) {
            for (const auto& ta : ia.texts) {
              auto* entry = getOrCreateTextTexture(ta.text, ta.font_size, updates);
              if (entry == nullptr || entry->tex == nullptr) {
                continue;
              }
              const float x0 = static_cast<float>(ta.position.x);
              const float y0 = static_cast<float>(ta.position.y);
              const float x1 = x0 + static_cast<float>(entry->width);
              const float y1 = y0 + static_cast<float>(entry->height);
              const float tr = static_cast<float>(ta.color.r) / 255.0f;
              const float tg = static_cast<float>(ta.color.g) / 255.0f;
              const float tb = static_cast<float>(ta.color.b) / 255.0f;
              const float tap = static_cast<float>(ta.color.a) / 255.0f;
              const size_t offset_bytes = text_vertex_data_.size() * sizeof(float);
              const float quad[6][8] = {
                  {x0, y0, 0.0f, 0.0f, tr, tg, tb, tap}, {x1, y0, 1.0f, 0.0f, tr, tg, tb, tap},
                  {x1, y1, 1.0f, 1.0f, tr, tg, tb, tap}, {x0, y0, 0.0f, 0.0f, tr, tg, tb, tap},
                  {x1, y1, 1.0f, 1.0f, tr, tg, tb, tap}, {x0, y1, 0.0f, 1.0f, tr, tg, tb, tap},
              };
              for (const auto& v : quad) {
                for (int k = 0; k < 8; ++k) {
                  text_vertex_data_.push_back(v[k]);
                }
              }
              text_draw_items_.push_back(TextDrawItem{entry->srb, offset_bytes});
            }
          }
        }
        const size_t t_needed = text_vertex_data_.size() * sizeof(float);
        if (t_needed > text_vbo_capacity_) {
          text_vbo_->destroy();
          text_vbo_capacity_ = std::max(t_needed * 2, text_vbo_capacity_);
          text_vbo_->setSize(static_cast<int>(text_vbo_capacity_));
          text_vbo_->create();
        }
        if (t_needed > 0) {
          updates->updateDynamicBuffer(text_vbo_, 0, static_cast<int>(t_needed), text_vertex_data_.data());
        }
      }

      overlays_dirty_ = false;
    }
    marker_vertex_count = marker_vertex_data_.size() / 6;  // 6 floats per vertex (vec2 + vec4)

    if (tex_width_ > 0 && tex_height_ > 0) {
      MarkerUbo ubo{};
      std::memcpy(ubo.view, view.constData(), sizeof(ubo.view));
      // Annotation overlays are authored in the displayed image's pixel space.
      // The decode pipeline rectifies a calibrated camera to its native (full)
      // resolution, so the texture dimensions are the correct normalization base.
      ubo.frame_size[0] = static_cast<float>(tex_width_);
      ubo.frame_size[1] = static_cast<float>(tex_height_);
      updates->updateDynamicBuffer(marker_uniform_buf_, 0, kMarkerUniformBufSize, &ubo);
    }
  }
  const size_t points_vertex_count = (points_pipeline_ != nullptr) ? points_vertex_data_.size() / 6 : 0;
  const size_t thick_vertex_count = (thick_pipeline_ != nullptr) ? thick_vertex_data_.size() / 6 : 0;

  cb->beginPass(rt, clear_color_, {1.0f, 0}, updates);
  if (pixel_layers_active_ && composite_pipeline_ != nullptr && !pixel_layer_textures_.empty()) {
    cb->setGraphicsPipeline(composite_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setScissor(imageScissor(view, output_size));
    for (auto& layer : pixel_layer_textures_) {
      if (layer.srb == nullptr || layer.width <= 0 || layer.height <= 0) {
        continue;
      }
      cb->setShaderResources(layer.srb);
      cb->draw(3);
    }
  } else {
    cb->setGraphicsPipeline(pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setScissor(imageScissor(view, output_size));
    cb->setShaderResources(srb_);
    cb->draw(3);
  }

  // Second draw call: vector overlays (markers) on top of the image, blended.
  // Draw order: image (already drawn) → fills (Triangles) → outlines (Lines).
  // Rationale: fills must be UNDER strokes so that LineLoop fill_color does not
  // hide its own outline, and circle outlines render on top of circle fills.
  // Viewport is reset on pipeline switch in some QRhi backends, so set explicitly.

  // Fills: kPoints quads + LineLoop fills + circle fills.
  if (points_pipeline_ != nullptr && points_vertex_count > 0 && tex_width_ > 0) {
    cb->setGraphicsPipeline(points_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setShaderResources(points_srb_);
    const QRhiCommandBuffer::VertexInput pinput(points_vbo_, 0);
    cb->setVertexInput(0, 1, &pinput);
    cb->draw(static_cast<quint32>(points_vertex_count));
  }

  // Outlines: line primitives (kLineList/kLineStrip/kLineLoop) + circle outlines, 1 px.
  if (marker_pipeline_ != nullptr && marker_vertex_count > 0 && tex_width_ > 0) {
    cb->setGraphicsPipeline(marker_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setShaderResources(marker_srb_);
    const QRhiCommandBuffer::VertexInput vinput(marker_vbo_, 0);
    cb->setVertexInput(0, 1, &vinput);
    cb->draw(static_cast<quint32>(marker_vertex_count));
  }

  // Thick outlines: lines/circles with thickness > 1.5, expanded to triangles.
  if (thick_pipeline_ != nullptr && thick_vertex_count > 0 && tex_width_ > 0) {
    cb->setGraphicsPipeline(thick_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    cb->setShaderResources(thick_srb_);
    const QRhiCommandBuffer::VertexInput tinput(thick_vbo_, 0);
    cb->setVertexInput(0, 1, &tinput);
    cb->draw(static_cast<quint32>(thick_vertex_count));
  }

  // Text labels — drawn last so they sit on top of all other overlays. Each
  // text uses its own pre-created SRB (one per cached texture) so the bindings
  // are stable between submission and execution. One draw call per text.
  if (text_pipeline_ != nullptr && !text_draw_items_.empty() && tex_width_ > 0) {
    cb->setGraphicsPipeline(text_pipeline_);
    cb->setViewport(
        QRhiViewport(0, 0, static_cast<float>(output_size.width()), static_cast<float>(output_size.height())));
    for (const auto& item : text_draw_items_) {
      cb->setShaderResources(item.srb);
      const QRhiCommandBuffer::VertexInput txi(text_vbo_, static_cast<quint32>(item.vbo_offset_bytes));
      cb->setVertexInput(0, 1, &txi);
      cb->draw(6);  // 1 quad = 2 triangles = 6 vertices
    }
  }

  cb->endPass();
}

void MediaViewerWidget::wheelEvent(QWheelEvent* e) {
  float old_zoom = zoom_;
  float delta = e->angleDelta().y() > 0 ? 1.1f : 1.0f / 1.1f;
  zoom_ = std::clamp(zoom_ * delta, 1.0f, 20.0f);

  if (zoom_ <= 1.0f) {
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
  } else {
    float mx = (2.0f * static_cast<float>(e->position().x()) / static_cast<float>(width()) - 1.0f);
    float my = (2.0f * static_cast<float>(e->position().y()) / static_cast<float>(height()) - 1.0f);
    pan_x_ += mx * (1.0f / zoom_ - 1.0f / old_zoom);
    pan_y_ += my * (1.0f / zoom_ - 1.0f / old_zoom);
  }

  update();
  emit zoomChanged(zoom_);
  if (point_inspector_enabled_.load(std::memory_order_relaxed) &&
      point_inspector_active_.load(std::memory_order_relaxed)) {
    refreshPointInspector();
  }
  e->accept();
}

void MediaViewerWidget::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) {
    hidePointInspector();
  }
  if (e->button() == Qt::LeftButton && zoom_ > 1.0f) {
    last_mouse_pos_ = e->position();
    e->accept();
  }
}

void MediaViewerWidget::mouseMoveEvent(QMouseEvent* e) {
  if ((e->buttons() & Qt::LeftButton) != 0 && zoom_ > 1.0f) {
    auto dx = static_cast<float>(e->position().x() - last_mouse_pos_.x()) / static_cast<float>(width()) * 2.0f / zoom_;
    auto dy = static_cast<float>(e->position().y() - last_mouse_pos_.y()) / static_cast<float>(height()) * 2.0f / zoom_;
    pan_x_ += dx;
    pan_y_ -= dy;
    last_mouse_pos_ = e->position();
    update();
    e->accept();
    return;
  }

  last_point_inspector_pos_ = e->position();
  point_inspector_active_.store(true, std::memory_order_relaxed);
  if (point_inspector_enabled_.load(std::memory_order_relaxed)) {
    refreshPointInspector();
  }
}

void MediaViewerWidget::mouseDoubleClickEvent(QMouseEvent* e) {
  resetView();
  e->accept();
}

void MediaViewerWidget::leaveEvent(QEvent* e) {
  point_inspector_active_.store(false, std::memory_order_relaxed);
  hidePointInspector();
  QRhiWidget::leaveEvent(e);
}

QMatrix4x4 MediaViewerWidget::buildViewTransform(QSize output_size) const {
  QMatrix4x4 m;
  float widget_aspect = static_cast<float>(output_size.width()) / static_cast<float>(output_size.height());
  float sx = 1.0f;
  float sy = 1.0f;
  if (frame_aspect_ > 0.0f) {
    if (widget_aspect > frame_aspect_) {
      sx = frame_aspect_ / widget_aspect;
    } else {
      sy = widget_aspect / frame_aspect_;
    }
  }
  m.scale(sx * zoom_, sy * zoom_);
  m.translate(pan_x_, pan_y_);
  return m;
}

void MediaViewerWidget::refreshPointInspector() {
  if (!point_inspector_enabled_.load(std::memory_order_relaxed) ||
      !point_inspector_active_.load(std::memory_order_relaxed)) {
    hidePointInspector();
    return;
  }

  DecodedFrame frame;
  {
    std::lock_guard lock(frame_mutex_);
    frame = inspector_frame_;
  }
  if (frame.isNull() || frame.width <= 0 || frame.height <= 0) {
    hidePointInspector();
    return;
  }

  const auto image_point = widgetPointToImagePixel(
      last_point_inspector_pos_, size(), QSize(frame.width, frame.height), zoom_, pan_x_, pan_y_);
  if (!image_point.has_value()) {
    hidePointInspector();
    return;
  }

  auto crop = extractRgbCrop(frame, image_point->x(), image_point->y(), kPointInspectorCropSize);
  if (crop.empty()) {
    hidePointInspector();
    return;
  }

  if (point_inspector_ == nullptr) {
    point_inspector_ = std::make_unique<PixelInspector>();
  }
  point_inspector_->updatePixel(std::move(crop), kPointInspectorCropSize, image_point->x(), image_point->y());
  point_inspector_->showNear(mapToGlobal(last_point_inspector_pos_.toPoint()));
}

void MediaViewerWidget::schedulePointInspectorRefresh() {
  if (!point_inspector_enabled_.load(std::memory_order_relaxed) ||
      !point_inspector_active_.load(std::memory_order_relaxed)) {
    return;
  }
  QMetaObject::invokeMethod(
      this,
      [this]() {
        if (point_inspector_enabled_.load(std::memory_order_relaxed) &&
            point_inspector_active_.load(std::memory_order_relaxed)) {
          refreshPointInspector();
        }
      },
      Qt::QueuedConnection);
}

void MediaViewerWidget::hidePointInspector() {
  if (point_inspector_ != nullptr) {
    point_inspector_->hideImmediately();
  }
}

QShader MediaViewerWidget::loadShader(const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    qWarning("Failed to load shader: %s", qPrintable(path));
    return {};
  }
  return QShader::fromSerialized(f.readAll());
}

MediaViewerWidget::TextEntry* MediaViewerWidget::getOrCreateTextTexture(
    const std::string& text, double font_size, QRhiResourceUpdateBatch* updates) {
  // Quantize font size to half-pixel units so near-identical sizes share a texture.
  const auto fq = static_cast<uint32_t>(font_size * 2.0 + 0.5);
  TextKey key{text, fq};
  auto it = text_cache_.find(key);
  if (it != text_cache_.end()) {
    return &it->second;
  }
  auto* r = rhi();
  if (r == nullptr) {
    return nullptr;
  }
  // Render glyph mask via QPainter into an Alpha8 image (white pixels = opaque).
  QFont font;
  font.setPixelSize(static_cast<int>(font_size + 0.5));
  QFontMetricsF fm(font);
  QString qtext = QString::fromUtf8(text.c_str(), static_cast<int>(text.size()));
  const int padding = 2;
  const int w = static_cast<int>(std::ceil(fm.horizontalAdvance(qtext))) + 2 * padding;
  const int h = static_cast<int>(std::ceil(fm.height())) + 2 * padding;
  if (w <= 0 || h <= 0) {
    return nullptr;
  }
  QImage img(w, h, QImage::Format_Alpha8);
  img.fill(0);
  {
    QPainter p(&img);
    p.setFont(font);
    p.setPen(QColor(255, 255, 255, 255));
    p.drawText(QPointF(padding, fm.ascent() + padding), qtext);
  }
  auto* tex = r->newTexture(QRhiTexture::R8, QSize(w, h));
  if (!tex->create()) {
    delete tex;
    return nullptr;
  }
  QRhiTextureSubresourceUploadDescription sub_desc(img);
  sub_desc.setSourceSize(QSize(w, h));
  updates->uploadTexture(tex, QRhiTextureUploadDescription({0, 0, sub_desc}));

  // Each TextEntry owns its own SRB so per-draw rebinding is impossible —
  // QRhi reads the SRB at submit time, not at the cb->setShaderResources call,
  // so reusing one SRB and mutating it between draws ends up with all draws
  // reading the LAST binding. One SRB per cached texture sidesteps that.
  auto* srb = r->newShaderResourceBindings();
  srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, marker_uniform_buf_),
      QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, tex, text_sampler_),
  });
  if (!srb->create()) {
    delete srb;
    delete tex;
    return nullptr;
  }

  TextEntry entry{tex, srb, w, h};
  auto [ins_it, _] = text_cache_.emplace(std::move(key), entry);
  return &ins_it->second;
}

}  // namespace PJ
