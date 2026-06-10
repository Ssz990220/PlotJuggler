#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPoint>
#include <QSize>
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

/// Tooltip magnifier for the current image pixel. It owns only the RGB crop copy;
/// callers keep frame storage and screen positioning policy.
class PixelInspector : public QWidget {
  Q_OBJECT

 public:
  explicit PixelInspector(QWidget* parent = nullptr);

  /// `crop_rgb` must contain at least crop_size*crop_size*3 bytes in RGB888
  /// row-major order; a shorter buffer silently clears and hides the tooltip.
  void updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y);
  /// Shows near the cursor and flips left/up when the tooltip would cross the
  /// available screen edge.
  void showNear(const QPoint& global_pos);
  void hideImmediately();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  [[nodiscard]] InspectorRgb cropPixel(int x, int y) const;

  std::vector<uint8_t> crop_data_;
  int crop_size_ = 0;
  int image_x_ = 0;
  int image_y_ = 0;
};

}  // namespace PJ
