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
#include <utility>

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

enum class LineWidth { kPoints10 = 0, kPoints15 = 1, kPoints20 = 2, kPoints30 = 3 };

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
  // Keyed by each curve's stable source_name (catalog key, == CurveDescriptor::name),
  // NOT its display title — so callers can correlate a plotted curve's colour with a
  // descriptor (e.g. the Filter Editor matching its source list to plot colours).
  [[nodiscard]] std::map<QString, QColor> curveColors() const;
  [[nodiscard]] CurveInfo* curveFromTitle(const QString& title);

  // The color at `index` in the built-in 8-color palette (index wraps modulo
  // the palette size). Lets callers that own a shared color counter (e.g. a
  // session-wide CurveColorRegistry) map an index to a color without depending
  // on this widget's per-instance nextColor() counter.
  [[nodiscard]] static QColor paletteColor(int index);

  // Session-wide override for the OpenGL-canvas choice. When set true (from the
  // --disable-opengl CLI flag in main.cpp), every plot constructed afterwards
  // uses the software raster canvas regardless of the Preferences::use_opengl
  // setting — without modifying that saved preference. The override is inactive
  // by default (so the saved preference decides); intended to be set once at
  // startup, before any plot is constructed.
  static void setOpenGlDisabledOverride(bool disabled);

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

  // Optional MANUAL y-axis limits. Each bound is independent (half-open pins are
  // supported: e.g. a fixed max with an auto-fit min). While pinned, every FIT/RESET
  // path (resetZoom, zoom-out, the vertical auto-fit, snapshot per-message refit,
  // layout restore) forces that bound instead of fitting it to the data — so a
  // snapshot plot fits X to the current message while Y stays put, and a restored
  // plot has a valid Y before any data. Manual wheel/drag zoom is NOT pinned (it
  // temporarily overrides); the next zoom-out/reset returns to the pinned range.
  void setFixedYRange(std::optional<double> y_min, std::optional<double> y_max);
  [[nodiscard]] std::optional<double> fixedYMin() const noexcept {
    return fixed_y_min_;
  }
  [[nodiscard]] std::optional<double> fixedYMax() const noexcept {
    return fixed_y_max_;
  }
  [[nodiscard]] bool hasFixedYRange() const noexcept {
    return fixed_y_min_.has_value() || fixed_y_max_.has_value();
  }

  void setAcceptDrops(bool accept);
  void overrideCurvesStyle(std::optional<CurveStyle> style);
  [[nodiscard]] std::optional<CurveStyle> overriddenCurvesStyle() const noexcept;
  // Sets the plot-level curve style: restyles every existing curve and is the
  // style new curves inherit (addCurve() applies curveStyle()). Style is a
  // property of the plot, not of an individual curve.
  void setDefaultStyle(CurveStyle default_style);
  [[nodiscard]] CurveStyle defaultCurveStyle() const noexcept;
  [[nodiscard]] CurveStyle curveStyle() const noexcept;
  void updateCurvesStyle();

  // Sets the plot-level line width: re-pens every existing curve and is the
  // width new curves inherit. Width is a property of the plot, not of an
  // individual curve.
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

  // Overlay the manual y-limits (if any) onto a proposed [min, max]: each bound the
  // caller proposed is kept unless that bound is pinned. The single seam the fit/reset
  // paths route their Y extent through so a pinned bound is honored everywhere.
  [[nodiscard]] std::pair<double, double> pinYRange(double proposed_min, double proposed_max) const;

 private:
  QwtPlotPimpl* plot_ = nullptr;
  bool xy_mode_ = false;
  QRectF max_zoom_rect_;
  bool keep_aspect_ratio_ = false;
  LineWidth line_width_ = LineWidth::kPoints10;
  int next_color_index_ = 0;
  // Manual y-axis limits; each bound independent (nullopt = auto-fit that bound).
  std::optional<double> fixed_y_min_;
  std::optional<double> fixed_y_max_;
};

}  // namespace PJ
