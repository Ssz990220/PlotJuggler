#pragma once

#include <qwt_series_data.h>

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QWidget>
#include <list>
#include <map>
#include <optional>

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

struct Range {
  double min = 0.0;
  double max = 0.0;
};

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

  virtual CurveInfo* addCurve(const QString& name, QwtSeriesData<QPointF>* series, QColor color = Qt::transparent);
  virtual void removeCurve(const QString& title);

  [[nodiscard]] const std::list<CurveInfo>& curveList() const noexcept;
  [[nodiscard]] std::list<CurveInfo>& curveList() noexcept;
  [[nodiscard]] bool isEmpty() const noexcept;
  [[nodiscard]] std::map<QString, QColor> curveColors() const;
  [[nodiscard]] CurveInfo* curveFromTitle(const QString& title);

  virtual void resetZoom();
  [[nodiscard]] virtual Range getVisualizationRangeX() const;
  [[nodiscard]] virtual Range getVisualizationRangeY(Range range_x) const;

  virtual void setModeXY(bool enable);
  [[nodiscard]] bool isXYPlot() const noexcept;

  void setLegendSize(int size);
  void setLegendAlignment(Qt::Alignment alignment);

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
  void widgetResized();

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

 private:
  QwtPlotPimpl* plot_ = nullptr;
  bool xy_mode_ = false;
  QRectF max_zoom_rect_;
  bool keep_aspect_ratio_ = false;
  LineWidth line_width_ = LineWidth::kPoints1_0;
  int next_color_index_ = 0;
};

}  // namespace PJ
