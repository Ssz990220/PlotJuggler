// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_widgets/pixel_inspector.h"

#include <QApplication>
#include <QPainter>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace PJ {

namespace {

constexpr int kZoomCellSize = 15;
constexpr int kInfoPanelWidth = 160;
constexpr int kPadding = 8;
constexpr int kSwatchSize = 40;
constexpr int kCursorOffset = 20;
constexpr int kFontSize = 11;
constexpr int kMinInfoHeight = 140;
constexpr int kRgbBytesPerPixel = 3;
constexpr int kRgbaBytesPerPixel = 4;

[[nodiscard]] uint8_t clampToByte(float value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

[[nodiscard]] InspectorRgb yuvToRgb(uint8_t y, uint8_t u, uint8_t v) {
  const float yf = static_cast<float>(y) / 255.0f;
  const float uf = static_cast<float>(u) / 255.0f - 0.5f;
  const float vf = static_cast<float>(v) / 255.0f - 0.5f;
  return {
      clampToByte((yf + 1.5748f * vf) * 255.0f),
      clampToByte((yf - 0.18732f * uf - 0.46812f * vf) * 255.0f),
      clampToByte((yf + 1.8556f * uf) * 255.0f),
  };
}

[[nodiscard]] bool hasExpectedStorage(const DecodedFrame& frame) {
  return frame.width > 0 && frame.height > 0 && frame.pixels != nullptr &&
         frame.pixels->size() >= expectedBufferSize(frame.width, frame.height, frame.format);
}

}  // namespace

std::optional<QPoint> widgetPointToImagePixel(
    QPointF widget_point, QSize widget_size, QSize image_size, float zoom, float pan_x, float pan_y) {
  if (widget_size.width() <= 0 || widget_size.height() <= 0 || image_size.width() <= 0 || image_size.height() <= 0 ||
      zoom <= 0.0f) {
    return std::nullopt;
  }

  float sx = zoom;
  float sy = zoom;
  const float image_aspect = static_cast<float>(image_size.width()) / static_cast<float>(image_size.height());
  const float widget_aspect = static_cast<float>(widget_size.width()) / static_cast<float>(widget_size.height());
  if (widget_aspect > image_aspect) {
    sx *= image_aspect / widget_aspect;
  } else {
    sy *= widget_aspect / image_aspect;
  }

  if (sx == 0.0f || sy == 0.0f) {
    return std::nullopt;
  }

  const float widget_u = static_cast<float>(widget_point.x()) / static_cast<float>(widget_size.width());
  const float widget_v = static_cast<float>(widget_point.y()) / static_cast<float>(widget_size.height());
  const float clip_x = widget_u * 2.0f - 1.0f;
  const float clip_y = (1.0f - widget_v) * 2.0f - 1.0f;

  const float u = (clip_x / sx - pan_x + 1.0f) * 0.5f;
  const float v = (1.0f - clip_y / sy + pan_y) * 0.5f;
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
    return std::nullopt;
  }

  const int x = std::clamp(static_cast<int>(u * static_cast<float>(image_size.width())), 0, image_size.width() - 1);
  const int y = std::clamp(static_cast<int>(v * static_cast<float>(image_size.height())), 0, image_size.height() - 1);
  return QPoint(x, y);
}

std::optional<InspectorRgb> pixelRgbAt(const DecodedFrame& frame, int x, int y) {
  if (!hasExpectedStorage(frame) || x < 0 || y < 0 || x >= frame.width || y >= frame.height) {
    return std::nullopt;
  }

  const uint8_t* data = frame.pixels->data();
  const size_t index = static_cast<size_t>(y) * static_cast<size_t>(frame.width) + static_cast<size_t>(x);
  switch (frame.format) {
    case PixelFormat::kRGB888: {
      const uint8_t* p = data + index * kRgbBytesPerPixel;
      return InspectorRgb{p[0], p[1], p[2]};
    }
    case PixelFormat::kBGR888: {
      const uint8_t* p = data + index * kRgbBytesPerPixel;
      return InspectorRgb{p[2], p[1], p[0]};
    }
    case PixelFormat::kRGBA8888: {
      const uint8_t* p = data + index * kRgbaBytesPerPixel;
      return InspectorRgb{p[0], p[1], p[2]};
    }
    case PixelFormat::kBGRA8888: {
      const uint8_t* p = data + index * kRgbaBytesPerPixel;
      return InspectorRgb{p[2], p[1], p[0]};
    }
    case PixelFormat::kYUV420P: {
      const int uv_w = (frame.width + 1) / 2;
      const int uv_h = (frame.height + 1) / 2;
      const size_t y_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
      const size_t uv_index = static_cast<size_t>(y / 2) * static_cast<size_t>(uv_w) + static_cast<size_t>(x / 2);
      if (uv_index >= static_cast<size_t>(uv_w) * static_cast<size_t>(uv_h)) {
        return std::nullopt;
      }
      return yuvToRgb(data[index], data[y_size + uv_index], data[y_size + static_cast<size_t>(uv_w) * uv_h + uv_index]);
    }
    case PixelFormat::kNV12: {
      const int uv_h = (frame.height + 1) / 2;
      const size_t y_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
      const size_t uv_index =
          (static_cast<size_t>(y / 2) * static_cast<size_t>(frame.width) + static_cast<size_t>(x / 2) * 2U);
      if (uv_index + 1 >= static_cast<size_t>(frame.width) * static_cast<size_t>(uv_h)) {
        return std::nullopt;
      }
      return yuvToRgb(data[index], data[y_size + uv_index], data[y_size + uv_index + 1]);
    }
    case PixelFormat::kMono8: {
      const uint8_t value = data[index];
      return InspectorRgb{value, value, value};
    }
    case PixelFormat::kMono16: {
      const size_t byte_index = index * 2;
      uint16_t value = 0;
      std::memcpy(&value, data + byte_index, sizeof(value));
      const uint8_t gray = static_cast<uint8_t>(value >> 8);
      return InspectorRgb{gray, gray, gray};
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> extractRgbCrop(const DecodedFrame& frame, int center_x, int center_y, int crop_size) {
  if (crop_size <= 0 || !hasExpectedStorage(frame)) {
    return {};
  }

  std::vector<uint8_t> crop(static_cast<size_t>(crop_size) * static_cast<size_t>(crop_size) * kRgbBytesPerPixel, 0);
  const int half = crop_size / 2;
  for (int row = 0; row < crop_size; ++row) {
    for (int col = 0; col < crop_size; ++col) {
      const int x = center_x - half + col;
      const int y = center_y - half + row;
      const auto rgb = pixelRgbAt(frame, x, y);
      if (!rgb.has_value()) {
        continue;
      }
      const size_t offset =
          (static_cast<size_t>(row) * static_cast<size_t>(crop_size) + static_cast<size_t>(col)) * kRgbBytesPerPixel;
      crop[offset] = rgb->r;
      crop[offset + 1] = rgb->g;
      crop[offset + 2] = rgb->b;
    }
  }
  return crop;
}

PixelInspector::PixelInspector(QWidget* parent) : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TransparentForMouseEvents);
}

void PixelInspector::updatePixel(std::vector<uint8_t> crop_rgb, int crop_size, int image_x, int image_y) {
  if (crop_size <= 0 ||
      crop_rgb.size() < static_cast<size_t>(crop_size) * static_cast<size_t>(crop_size) * kRgbBytesPerPixel) {
    crop_data_.clear();
    crop_size_ = 0;
    hideImmediately();
    return;
  }

  crop_data_ = std::move(crop_rgb);
  crop_size_ = crop_size;
  image_x_ = image_x;
  image_y_ = image_y;

  const int zoom_area = crop_size_ * kZoomCellSize;
  setFixedSize(zoom_area + kInfoPanelWidth + kPadding * 3, std::max(zoom_area, kMinInfoHeight) + kPadding * 2);
  update();
}

void PixelInspector::showNear(const QPoint& global_pos) {
  QScreen* screen = QApplication::screenAt(global_pos);
  if (screen == nullptr) {
    screen = QApplication::primaryScreen();
  }
  if (screen == nullptr) {
    return;
  }

  const QRect screen_rect = screen->availableGeometry();
  int x = global_pos.x() + kCursorOffset;
  int y = global_pos.y() + kCursorOffset;
  if (x + width() > screen_rect.right()) {
    x = global_pos.x() - width() - kCursorOffset;
  }
  if (y + height() > screen_rect.bottom()) {
    y = global_pos.y() - height() - kCursorOffset;
  }
  move(x, y);

  if (!isVisible()) {
    show();
  } else {
    raise();
  }
}

void PixelInspector::hideImmediately() {
  hide();
}

void PixelInspector::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.fillRect(rect(), QColor(30, 30, 30, 240));
  if (crop_data_.empty() || crop_size_ <= 0) {
    return;
  }

  const int zoom_area = crop_size_ * kZoomCellSize;
  const int grid_x = kPadding;
  const int grid_y = kPadding;
  for (int row = 0; row < crop_size_; ++row) {
    for (int col = 0; col < crop_size_; ++col) {
      const auto pixel = cropPixel(col, row);
      painter.fillRect(
          grid_x + col * kZoomCellSize, grid_y + row * kZoomCellSize, kZoomCellSize, kZoomCellSize,
          QColor(pixel.r, pixel.g, pixel.b));
    }
  }

  painter.setPen(QPen(QColor(80, 80, 80, 120), 1));
  for (int i = 0; i <= crop_size_; ++i) {
    painter.drawLine(grid_x + i * kZoomCellSize, grid_y, grid_x + i * kZoomCellSize, grid_y + zoom_area);
    painter.drawLine(grid_x, grid_y + i * kZoomCellSize, grid_x + zoom_area, grid_y + i * kZoomCellSize);
  }

  const int half = crop_size_ / 2;
  painter.setPen(QPen(Qt::white, 2));
  painter.drawRect(grid_x + half * kZoomCellSize, grid_y + half * kZoomCellSize, kZoomCellSize, kZoomCellSize);

  const auto center = cropPixel(half, half);
  int info_x = grid_x + zoom_area + kPadding;
  int info_y = grid_y;
  QFont mono(QStringLiteral("monospace"), kFontSize);
  painter.setFont(mono);
  painter.setPen(Qt::white);

  constexpr int line_h = 20;
  painter.drawText(info_x, info_y + line_h, QStringLiteral("Position: %1, %2").arg(image_x_).arg(image_y_));
  info_y += line_h + 8;
  painter.drawText(
      info_x, info_y + line_h,
      QStringLiteral("RGB: %1, %2, %3")
          .arg(static_cast<int>(center.r), 3)
          .arg(static_cast<int>(center.g), 3)
          .arg(static_cast<int>(center.b), 3));
  info_y += line_h + 4;

  const QString hex = QStringLiteral("#%1%2%3")
                          .arg(static_cast<int>(center.r), 2, 16, QChar('0'))
                          .arg(static_cast<int>(center.g), 2, 16, QChar('0'))
                          .arg(static_cast<int>(center.b), 2, 16, QChar('0'))
                          .toUpper();
  painter.drawText(info_x, info_y + line_h, hex);
  info_y += line_h + 12;

  painter.fillRect(info_x, info_y, kSwatchSize, kSwatchSize, QColor(center.r, center.g, center.b));
  painter.setPen(QPen(QColor(100, 100, 100), 1));
  painter.drawRect(info_x, info_y, kSwatchSize, kSwatchSize);
}

InspectorRgb PixelInspector::cropPixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= crop_size_ || y >= crop_size_ || crop_data_.empty()) {
    return {};
  }
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(crop_size_) + static_cast<size_t>(x)) * kRgbBytesPerPixel;
  if (offset + 2 >= crop_data_.size()) {
    return {};
  }
  return InspectorRgb{crop_data_[offset], crop_data_[offset + 1], crop_data_[offset + 2]};
}

}  // namespace PJ
