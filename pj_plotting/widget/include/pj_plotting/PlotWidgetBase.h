#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_series_data.h>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <list>
#include <map>
#include <optional>

#include "pj_base/types.hpp"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QwtPlot;
class QwtPlotCurve;
class QwtPlotMarker;

namespace PJ {

class PlotLegend;
class PlotMagnifier;
class PlotPanner;
class PlotZoomer;

enum class LineWidth { kPoints1_0 = 0, kPoints1_5 = 1, kPoints2_0 = 2, kPoints3_0 = 3 };

[[nodiscard]] double lineWidthValue(LineWidth line_width) noexcept;
[[nodiscard]] double dotWidthValue(LineWidth line_width) noexcept;

class PlotWidgetBase : public QWidget {
  Q_OBJECT
 public:
  enum CurveStyle { kLines, kDots, kLinesAndDots, kSticks, kSteps, kStepsInverted };

  struct CurveInfo {
    QString source_name;
    QwtPlotCurve* curve = nullptr;
    QwtPlotMarker* marker = nullptr;
  };

  explicit PlotWidgetBase(QWidget* parent = nullptr);
  ~PlotWidgetBase() override;

  virtual CurveInfo* addCurve(
      const QString& name, QwtSeriesData<QPointF>* series, QColor color = Qt::transparent,
      const QString& display_name = {});
  virtual void removeCurve(const QString& title);

  [[nodiscard]] const std::list<CurveInfo>& curveList() const noexcept;
  [[nodiscard]] std::list<CurveInfo>& curveList() noexcept;
  [[nodiscard]] bool isEmpty() const noexcept;
  [[nodiscard]] std::map<QString, QColor> curveColors() const;
  [[nodiscard]] CurveInfo* curveFromTitle(const QString& title);

  // The color at `index` in the built-in 8-color palette (index wraps modulo
  // the palette size). Lets callers that own a shared color counter (e.g. a
  // session-wide CurveColorRegistry) map an index to a color without depending
  // on this widget's per-instance nextColor() counter.
  [[nodiscard]] static QColor paletteColor(int index);

  virtual void resetZoom();
  [[nodiscard]] virtual Range<double> getVisualizationRangeX() const;
  [[nodiscard]] virtual Range<double> getVisualizationRangeY(Range<double> range_x) const;

  virtual void setModeXY(bool enable);
  [[nodiscard]] bool isXYPlot() const noexcept;

  void setLegendSize(int size);
  void setLegendAlignment(Qt::Alignment alignment);
  void setLegendVisible(bool visible);
  [[nodiscard]] bool legendVisible() const noexcept;

  void setGridVisible(bool visible);
  [[nodiscard]] bool gridVisible() const noexcept;

  void setZoomEnabled(bool enabled);
  [[nodiscard]] bool isZoomEnabled() const noexcept;
  void setSwapZoomPan(bool swapped);

  [[nodiscard]] QRectF currentBoundingRect() const;
  [[nodiscard]] QRectF maxZoomRect() const noexcept;

  [[nodiscard]] bool keepRatioXY() const noexcept;
  void setKeepRatioXY(bool active);

  void setAcceptDrops(bool accept);
  void overrideCurvesStyle(std::optional<CurveStyle> style);
  [[nodiscard]] std::optional<CurveStyle> overriddenCurvesStyle() const noexcept;
  void setDefaultStyle(CurveStyle default_style);
  [[nodiscard]] CurveStyle defaultCurveStyle() const noexcept;
  [[nodiscard]] CurveStyle curveStyle() const noexcept;
  void updateCurvesStyle();

  void setLineWidth(LineWidth width);
  [[nodiscard]] LineWidth lineWidth() const noexcept {
    return line_width_;
  }

  // Installs `filter` on the canvas and every visible axis scale widget,
  // so the caller sees Enter/Leave across the whole chart surface (Qt
  // does not bubble those events to ancestors).
  void installHoverFilter(QObject* filter);

 public slots:
  void replot();
  virtual void removeAllCurves();

 signals:
  void curveListChanged();
  void viewResized(const QRectF& rect);
  void dragEnterSignal(QDragEnterEvent* event);
  void dragLeaveSignal(QDragLeaveEvent* event);
  void dropSignal(QDropEvent* event);
  void legendSizeChanged(int new_size);

 protected:
  class QwtPlotPimpl;

  void setStyle(QwtPlotCurve* curve, CurveStyle style);
  QColor nextColor();

  [[nodiscard]] QwtPlot* qwtPlot();
  [[nodiscard]] const QwtPlot* qwtPlot() const;
  [[nodiscard]] PlotLegend* legend();
  [[nodiscard]] PlotZoomer* zoomer();
  [[nodiscard]] PlotMagnifier* magnifier();
  [[nodiscard]] PlotPanner* panner1();
  [[nodiscard]] PlotPanner* panner2();

  void updateMaximumZoomArea();
  bool eventFilter(QObject* obj, QEvent* event) override;

  // Corrects rect to the canvas aspect ratio (when XY + keepRatioXY) and
  // applies it to the axes. Caller decides whether to replot. Pass the rect
  // explicitly: after a magnifier/panner change, currentBoundingRect() is
  // still stale (those emit before they replot).
  void applyRectKeepingRatio(QRectF rect);

  // Applies a rect to the axes (min/max-safe, so it tolerates the normalized
  // rect that applyKeepAspectRatio produces). Caller decides whether to replot.
  void applyRectToAxes(const QRectF& rect);

 private:
  QwtPlotPimpl* plot_ = nullptr;
  bool xy_mode_ = false;
  QRectF max_zoom_rect_;
  bool keep_aspect_ratio_ = false;
  LineWidth line_width_ = LineWidth::kPoints1_0;
  int next_color_index_ = 0;
};

}  // namespace PJ
