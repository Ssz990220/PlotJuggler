#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <rhi/qrhi.h>

#include <QColor>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QRhiWidget>
#include <QSize>
#include <QWheelEvent>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_scene2d_core/media_frame.h"
#include "pj_scene2d_core/scene_frame.h"

namespace PJ {

class MediaSource;
class PixelInspector;

/// GPU-accelerated scene/video viewer using QRhiWidget.
///
/// Attach a MediaSource with setMediaSource(), then call setTimestamp() on each
/// application tick. The widget polls the source in render() via takeFrame().
///
/// Supports YUV420P (3-plane BT.709 shader), packed RGB/RGBA DecodedFrame
/// payloads, and MediaFrame.pixel_layers alpha-composited in order.
/// SceneFrame overlays (points/lines/circles/text) are tessellated CPU-side and
/// drawn above the image; see ARCHITECTURE.md §7.1.
///
/// Zoom (mouse wheel, cursor-anchored) and pan (mouse drag) via a view
/// transform matrix in the vertex shader. See REQUIREMENTS.md §4.7.
class MediaViewerWidget : public QRhiWidget {
  Q_OBJECT
  Q_PROPERTY(QColor clearColor READ clearColor WRITE setClearColor)

 public:
  explicit MediaViewerWidget(QWidget* parent = nullptr);
  ~MediaViewerWidget() override;

  /// Attach a MediaSource. The widget does NOT take ownership.
  /// Call setTimestamp() to drive the source; render() polls takeFrame().
  void setMediaSource(MediaSource* source);

  /// Forward a timestamp to the attached MediaSource.
  /// No-op if no source is attached.
  void setTimestamp(int64_t ts_ns);

  /// Reset zoom to 1x and pan to origin.
  void resetView();

  void setClearColor(const QColor& color);
  [[nodiscard]] QColor clearColor() const;

  /// Enables the hover pixel magnifier used by the global "Show point" toggle.
  void setPointInspectorEnabled(bool enabled);
  [[nodiscard]] bool pointInspectorEnabled() const noexcept;

 signals:
  void zoomChanged(float zoom);

 protected:
  void initialize(QRhiCommandBuffer* cb) override;
  void render(QRhiCommandBuffer* cb) override;
  void releaseResources() override;

  void wheelEvent(QWheelEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  [[nodiscard]] QMatrix4x4 buildViewTransform(QSize output_size) const;
  [[nodiscard]] bool hasRetainedUploadableFrameLocked() const;
  void resetPendingPixelLayers();
  void refreshPointInspector();
  void schedulePointInspectorRefresh();
  void hidePointInspector();
  static QShader loadShader(const QString& path);
  // Get-or-create the glyph mask texture for a given (text, font_size). Renders
  // via QPainter on first miss and uploads as an R8 QRhiTexture. The texture
  // pointer is owned by text_cache_; never delete the returned pointer.
  struct TextEntry;
  TextEntry* getOrCreateTextTexture(const std::string& text, double font_size, QRhiResourceUpdateBatch* updates);
  // Delete every TextEntry's owned QRhi resources and clear the cache + per-frame
  // draw items. Caller must hold any locks that protect text_cache_ /
  // text_draw_items_; called by setMediaSource() (under frame_mutex_) and by
  // releaseResources() (Qt has already stopped rendering).
  void clearTextCache();

  // Selects the YUV→RGB shader path. Values must match the `pixelFormat`
  // uniform contract in shaders/yuv_to_rgb.frag. kNV12 is RESERVED: the shader
  // branch exists, but no decoder emits NV12 and the upload support gate
  // rejects it — wire the two-plane upload before producing it.
  enum class TexturePathFormat : int32_t {
    kYUV420P = 0,
    kNV12 = 1,
    kRGBA = 2,
  };

  // Single definition of the PixelFormat -> shader-path projection: planar YUV
  // stays planar; every packed RGB/BGR/mono layout is CPU-converted and lands
  // on the RGBA path.
  [[nodiscard]] static TexturePathFormat texturePathFor(PixelFormat format) noexcept;
  [[nodiscard]] static bool isUploadablePixelFormat(PixelFormat format) noexcept;

  struct TextureLayerResources {
    QRhiTexture* tex_y = nullptr;
    QRhiTexture* tex_u = nullptr;
    QRhiTexture* tex_v = nullptr;
    QRhiBuffer* uniform_buf = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    int width = 0;
    int height = 0;
    TexturePathFormat format = TexturePathFormat::kRGBA;
    float opacity = 1.0f;
  };

  struct OverlayPipeline {
    QRhiGraphicsPipeline* pipeline = nullptr;
    QRhiBuffer* vbo = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;
    size_t vbo_capacity = 0;
    std::vector<float> vertex_data;
  };

  void clearPixelLayerTextures();
  void destroyTextureLayer(TextureLayerResources& layer);
  bool ensureTextureLayer(TextureLayerResources& layer);
  bool uploadDecodedFrameToTexture(
      const DecodedFrame& frame, TextureLayerResources& layer, QRhiResourceUpdateBatch* updates);
  void updateTextureLayerUniform(
      TextureLayerResources& layer, const QMatrix4x4& view, QRhiResourceUpdateBatch* updates) const;
  void destroyOverlayPipeline(OverlayPipeline& overlay);
  bool createOverlayVbo(OverlayPipeline& overlay, size_t initial_capacity);
  bool createUniformOverlaySrb(OverlayPipeline& overlay);
  bool createOverlayGraphicsPipeline(
      OverlayPipeline& overlay, const QShader& vert, const QShader& frag, QRhiGraphicsPipeline::Topology topology,
      const QRhiVertexInputLayout& input_layout, const char* failure_message);
  void uploadOverlayVertexData(OverlayPipeline& overlay, QRhiResourceUpdateBatch* updates);

  // Pipeline for YUV→RGB shader (video frames)
  QRhi* rhi_cached_ = nullptr;
  QRhiGraphicsPipeline* pipeline_ = nullptr;
  QRhiGraphicsPipeline* composite_pipeline_ = nullptr;
  QRhiSampler* sampler_ = nullptr;
  TextureLayerResources base_texture_;

  // MediaSource (not owned)
  MediaSource* media_source_ = nullptr;

  // Last CPU-side frame. `has_pending_` marks whether it still needs an
  // upload; the data itself is intentionally retained after upload so QRhi
  // resource recreation can restore the latest visible image.
  std::mutex frame_mutex_;
  DecodedFrame pending_decoded_;  // YUV420P or RGB frame
  DecodedFrame inspector_frame_;
  bool has_pending_ = false;
  std::vector<PixelLayer> pending_pixel_layers_;
  bool has_pending_pixel_layers_ = false;
  bool pixel_layers_active_ = false;

  int tex_width_ = 0;
  int tex_height_ = 0;
  float frame_aspect_ = 0.0f;

  std::vector<TextureLayerResources> pixel_layer_textures_;
  std::vector<uint8_t> rgba_repack_buffer_;

  float zoom_ = 1.0f;
  float pan_x_ = 0.0f;
  float pan_y_ = 0.0f;
  QPointF last_mouse_pos_;
  QPointF last_point_inspector_pos_;
  QColor clear_color_{Qt::white};
  std::unique_ptr<PixelInspector> point_inspector_;
  std::atomic_bool point_inspector_enabled_{false};
  std::atomic_bool point_inspector_active_{false};

  // Uniform buffer layout (std140):
  // mat4 viewTransform  (64 bytes, offset 0)
  // mat4 colorMatrix    (64 bytes, offset 64)
  // int  pixelFormat    (4 bytes, offset 128)
  // padding             (12 bytes)
  static constexpr int kUniformBufSize = 144;

  // ----- Vector overlay pipeline (markers / annotations) -----
  // Second QRhi pipeline that draws line primitives on top of the image
  // pass, sharing the viewTransform so markers track pan/zoom/letterbox.
  QRhiBuffer* marker_uniform_buf_ = nullptr;
  OverlayPipeline marker_overlay_;
  std::vector<SceneFrame> last_overlays_;  ///< persisted across renders
  bool overlays_dirty_ = false;            ///< rebuild VBO on next render

  // ----- kPoints quad pipeline (solid filled squares for kPoints topology) -----
  // Third QRhi pipeline (Triangles topology) sharing marker_uniform_buf_ but
  // with its own SRB and VBO. Each kPoints point becomes 2 triangles centred
  // on the point with side = thickness.
  OverlayPipeline points_overlay_;

  // ----- Thick lines pipeline (Triangles topology, perpendicular expansion) -----
  // Fourth QRhi pipeline. Used when PointsAnnotation.thickness > 1.5 (line
  // primitives) or when CircleAnnotation.thickness > 1.5. Each segment expands
  // CPU-side to 2 triangles forming a rectangle of width = thickness.
  OverlayPipeline thick_overlay_;

  // ----- Text pipeline (Triangles, textured quads with QPainter masks) -----
  // Fifth QRhi pipeline. One textured quad per TextAnnotation; texture is an
  // R8 alpha mask painted by QPainter, the per-vertex color provides the tint.
  // Cache key = (text, font_size_q): two labels with same text+size but different
  // colors share the same texture (color applied at fragment time).
  OverlayPipeline text_overlay_;                 // pipeline layout SRB uses the placeholder texture
  QRhiTexture* text_placeholder_tex_ = nullptr;  // owned, lives until releaseResources
  QRhiSampler* text_sampler_ = nullptr;

  struct TextKey {
    std::string text;
    uint32_t font_size_q = 0;
    bool operator==(const TextKey& o) const noexcept {
      return font_size_q == o.font_size_q && text == o.text;
    }
  };
  struct TextKeyHash {
    size_t operator()(const TextKey& k) const noexcept {
      return std::hash<std::string>{}(k.text) ^ (static_cast<size_t>(k.font_size_q) * 0x9e3779b9u);
    }
  };
  struct TextEntry {
    QRhiTexture* tex = nullptr;
    QRhiShaderResourceBindings* srb = nullptr;  // Owns its own binding to `tex`.
    int width = 0;
    int height = 0;
  };
  std::unordered_map<TextKey, TextEntry, TextKeyHash> text_cache_;
  // Per-text quad metadata captured at rebuild time, consumed at draw time.
  struct TextDrawItem {
    QRhiShaderResourceBindings* srb;  // pointer borrowed from text_cache_
    size_t vbo_offset_bytes;
  };
  std::vector<TextDrawItem> text_draw_items_;

  // Uniform layout for marker pipeline (std140). frameSize is vec4 (only .xy
  // used) instead of vec2 to dodge a std140 alignment quirk in the OpenGL
  // backend; see scene_lines.vert for context.
  struct alignas(16) MarkerUbo {
    float view[16];
    float frame_size[4];
  };
  static constexpr int kMarkerUniformBufSize = sizeof(MarkerUbo);
};

}  // namespace PJ
