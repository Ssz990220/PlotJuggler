#pragma once

#include <QPoint>
#include <QSize>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

#include "pj_scene2d_core/decoded_frame.h"

namespace PJ {

struct InspectorRgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;

  [[nodiscard]] bool operator==(const InspectorRgb& other) const noexcept {
    return r == other.r && g == other.g && b == other.b;
  }
};

[[nodiscard]] std::optional<QPoint> widgetPointToImagePixel(
    QPointF widget_point, QSize widget_size, QSize image_size, float zoom, float pan_x, float pan_y);

[[nodiscard]] std::optional<InspectorRgb> pixelRgbAt(const DecodedFrame& frame, int x, int y);

[[nodiscard]] std::vector<uint8_t> extractRgbCrop(const DecodedFrame& frame, int center_x, int center_y, int crop_size);

class PixelInspector : public QWidget {
  Q_OBJECT

 public:
  explicit PixelInspector(QWidget* parent = nullptr);

  void updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y);
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
