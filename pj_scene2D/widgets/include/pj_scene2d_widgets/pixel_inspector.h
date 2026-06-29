#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPoint>
#include <QSize>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

/// Display-space RGB sample used by the inspector; always normalized to RGB888,
/// regardless of the source frame's packed, planar, or mono storage format.
struct InspectorRgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  [[nodiscard]] bool operator==(const InspectorRgb& other) const noexcept {
    return r == other.r && g == other.g && b == other.b;
  }
};

/// Maps a widget point to an image pixel using MediaViewerWidget::buildViewTransform()
/// zoom/pan semantics. Returns nullopt for invalid sizes/zoom or off-image points;
/// pan is normalized clip-space offset (+X moves image right, +Y moves it up).
[[nodiscard]] std::optional<QPoint> widgetPointToImagePixel(
    QPointF widget_point, QSize widget_size, QSize image_size, float zoom, float pan_x, float pan_y);

/// Samples one image pixel as RGB, converting supported YUV/mono/BGR layouts.
/// Returns nullopt for invalid storage, unsupported bounds, or off-image points.
[[nodiscard]] std::optional<InspectorRgb> pixelRgbAt(const DecodedFrame& frame, int x, int y);

/// Extracts a row-major RGB888 crop centered on the image pixel. Pixels outside
/// the frame remain zero-filled; invalid frames or non-positive crop sizes return {}.
[[nodiscard]] std::vector<uint8_t> extractRgbCrop(const DecodedFrame& frame, int center_x, int center_y, int crop_size);

/// Samples metric depth (metres) at (x,y) from a kDepthR32F frame. Returns nullopt
/// for a non-depth frame, out-of-bounds (x,y), or a no-data pixel (non-finite or
/// <= 0) — the same validity rule DepthPipelineSource and the depth shader's
/// `!(d > 0)` test apply, so the readout agrees pixel-for-pixel with the display.
[[nodiscard]] std::optional<float> depthMetersAt(const DecodedFrame& frame, int x, int y);

/// Reproduces the depth media shader's on-screen colour for the inspector swatch:
/// normalize `depth_m` by [near_m, far_m] (far-near guarded by 1e-6 as on the GPU),
/// optionally invert, then map through `params.colormap` via pj_widgets colorFor()
/// — the same colormap source the GPU LUT is built from, so the swatch matches the
/// pixel (up to 256-entry LUT quantization). `depth_m` must be a valid sample
/// (callers check depthMetersAt() first).
[[nodiscard]] InspectorRgb depthColormapColor(float depth_m, const DepthColorParams& params);

/// Tooltip magnifier for the current image pixel. It owns only the RGB crop copy;
/// callers keep frame storage and screen positioning policy. Has two render modes:
/// the default RGB mode (zoom grid + RGB readout, fed by updatePixel) and a depth
/// mode (position + metric depth + colormap swatch, no zoom grid, fed by updateDepth).
class PixelInspector : public QWidget {
  Q_OBJECT

 public:
  explicit PixelInspector(QWidget* parent = nullptr);

  /// `crop_rgb` must contain at least crop_size*crop_size*3 bytes in RGB888
  /// row-major order; a shorter buffer silently clears and hides the tooltip.
  /// Switches the inspector to RGB mode.
  void updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y);

  /// Switches the inspector to depth mode: shows `Position: x, y` and the metric
  /// depth (or "— (no data)" when `depth_m` is nullopt), plus a swatch of the
  /// on-screen colormapped colour. No zoom grid — the colormap output carries no
  /// per-pixel RGB worth magnifying. `params` supplies near/far/invert/colormap so
  /// the swatch reproduces the displayed colour via depthColormapColor().
  void updateDepth(int image_x, int image_y, std::optional<float> depth_m, const DepthColorParams& params);

  /// Shows near the cursor and flips left/up when the tooltip would cross the
  /// available screen edge.
  void showNear(const QPoint& global_pos);
  void hideImmediately();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  /// Active render mode; selected by updatePixel (Rgb) / updateDepth (Depth).
  enum class Mode { Rgb, Depth };

  [[nodiscard]] InspectorRgb cropPixel(int x, int y) const;
  void paintRgb(QPainter& painter);
  void paintDepth(QPainter& painter);
  /// The "Depth: …" readout line: the metric value, or "— (no data)" for a
  /// no-data pixel. Shared by updateDepth (to size the tooltip to fit) and
  /// paintDepth (to draw it), so the two never disagree.
  [[nodiscard]] QString depthValueText() const;

  Mode mode_ = Mode::Rgb;

  // RGB-mode state.
  std::vector<uint8_t> crop_data_;
  int crop_size_ = 0;

  // Depth-mode state.
  std::optional<float> depth_m_;   ///< nullopt == no-data pixel.
  DepthColorParams depth_params_;  ///< near/far/invert/colormap for the swatch.

  // Shared.
  int image_x_ = 0;
  int image_y_ = 0;
};

}  // namespace PJ
