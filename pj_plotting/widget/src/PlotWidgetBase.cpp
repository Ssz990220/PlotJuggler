// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotWidgetBase.h"

#include <qwt_axis.h>
#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_layout.h>
#include <qwt_plot_marker.h>
#include <qwt_plot_opengl_canvas.h>
#include <qwt_scale_engine.h>
#include <qwt_scale_map.h>
#include <qwt_scale_widget.h>
#include <qwt_symbol.h>

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPen>
#include <QSettings>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotMagnifier.h"
#include "pj_plotting/PlotPanner.h"
#include "pj_plotting/PlotZoomer.h"

namespace PJ {
namespace {

[[nodiscard]] bool isUsableRect(const QRectF& rect) noexcept {
  return rect.width() >= 0.0 && rect.height() >= 0.0 && std::isfinite(rect.left()) && std::isfinite(rect.right()) &&
         std::isfinite(rect.top()) && std::isfinite(rect.bottom());
}

[[nodiscard]] QColor colorFromIndex(int index) {
  static const std::array<QColor, 8> kColors = {
      QColor("#1f77b4"), QColor("#d62728"), QColor("#1ac938"), QColor("#ff7f0e"),
      QColor("#f14cc1"), QColor("#9467bd"), QColor("#17becf"), QColor("#bcbd22"),
  };
  return kColors[static_cast<std::size_t>(index) % kColors.size()];
}

}  // namespace

double lineWidthValue(LineWidth line_width) noexcept {
  constexpr std::array<double, 4> kLineWidths = {1.0, 1.5, 2.0, 3.0};
  return 1.4 * kLineWidths[static_cast<std::size_t>(line_width)];
}

double dotWidthValue(LineWidth line_width) noexcept {
  return (lineWidthValue(line_width) * 1.5) + 2.0;
}

class PlotWidgetBase::QwtPlotPimpl : public QwtPlot {
 public:
  QwtPlotPimpl(
      PlotWidgetBase* parent_widget, QWidget* canvas_widget, std::function<void(const QRectF&)> resized_callback,
      std::function<void(QEvent*)> event_callback)
      : QwtPlot(nullptr),
        resized_callback(std::move(resized_callback)),
        event_callback(std::move(event_callback)),
        parent(parent_widget) {
    setCanvas(canvas_widget);
    legend = new PlotLegend(this);
    grid = new QwtPlotGrid();
    grid->enableX(false);
    grid->enableXMin(false);
    grid->enableY(false);
    grid->enableYMin(false);
    grid->setMajorPen(QPen(QColor(150, 150, 150), 0.0, Qt::DashLine));
    grid->setMinorPen(QPen(QColor(210, 210, 210), 0.0, Qt::DotLine));
    grid->attach(this);
    magnifier = new PlotMagnifier(canvas_widget);
    panner1 = new PlotPanner(canvas_widget);
    panner2 = new PlotPanner(canvas_widget);
    zoomer = new PlotZoomer(canvas_widget);

    zoomer->setRubberBandPen(QPen(QColor(Qt::red), 1, Qt::DotLine));
    zoomer->setTrackerPen(QPen(QColor(Qt::green), 1, Qt::DotLine));
    zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);

    magnifier->setAxisEnabled(QwtPlot::xTop, false);
    magnifier->setAxisEnabled(QwtPlot::yRight, false);
    magnifier->setZoomInKey(Qt::Key_Plus, Qt::ControlModifier);
    magnifier->setZoomOutKey(Qt::Key_Minus, Qt::ControlModifier);
    magnifier->setMouseButton(Qt::NoButton);

    panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
    panner2->setMouseButton(Qt::MiddleButton, Qt::NoModifier);

    connect(zoomer, &PlotZoomer::zoomed, this, [this](const QRectF& rect) { this->resized_callback(rect); });
    connect(magnifier, &PlotMagnifier::rescaled, this, [this](const QRectF& rect) {
      this->resized_callback(rect);
      replot();
    });
    connect(panner1, &PlotPanner::rescaled, this, [this](const QRectF& rect) { this->resized_callback(rect); });
    connect(panner2, &PlotPanner::rescaled, this, [this](const QRectF& rect) { this->resized_callback(rect); });

    axisWidget(QwtPlot::xBottom)->installEventFilter(parent);
    axisWidget(QwtPlot::yLeft)->installEventFilter(parent);
    canvas()->installEventFilter(parent);
  }

  ~QwtPlotPimpl() override {
    axisWidget(QwtPlot::xBottom)->removeEventFilter(parent);
    axisWidget(QwtPlot::yLeft)->removeEventFilter(parent);
    canvas()->removeEventFilter(parent);
    setCanvas(nullptr);
  }

  [[nodiscard]] QRectF canvasBoundingRect() const {
    QRectF rect;
    rect.setBottom(canvasMap(QwtPlot::yLeft).s1());
    rect.setTop(canvasMap(QwtPlot::yLeft).s2());
    rect.setLeft(canvasMap(QwtPlot::xBottom).s1());
    rect.setRight(canvasMap(QwtPlot::xBottom).s2());
    return rect;
  }

  void resizeEvent(QResizeEvent* event) override {
    QwtPlot::resizeEvent(event);
    resized_callback(canvasBoundingRect());
  }

  void dragEnterEvent(QDragEnterEvent* event) override {
    event_callback(event);
  }
  void dragLeaveEvent(QDragLeaveEvent* event) override {
    event_callback(event);
  }
  void dropEvent(QDropEvent* event) override {
    event_callback(event);
  }

  PlotLegend* legend = nullptr;
  QwtPlotGrid* grid = nullptr;
  PlotMagnifier* magnifier = nullptr;
  PlotPanner* panner1 = nullptr;
  PlotPanner* panner2 = nullptr;
  PlotZoomer* zoomer = nullptr;
  std::function<void(const QRectF&)> resized_callback;
  std::function<void(QEvent*)> event_callback;
  PlotWidgetBase* parent = nullptr;
  std::list<CurveInfo> curve_list;
  std::optional<CurveStyle> overridden_curve_style;
  CurveStyle default_curve_style = kLines;
  bool zoom_enabled = true;
};

PlotWidgetBase::PlotWidgetBase(QWidget* parent) : QWidget(parent) {
  auto on_view_resized = [this](const QRectF& rect) { emit viewResized(rect); };
  auto on_event = [this](QEvent* event) {
    if (auto* drag_enter = dynamic_cast<QDragEnterEvent*>(event)) {
      emit dragEnterSignal(drag_enter);
    } else if (auto* drag_leave = dynamic_cast<QDragLeaveEvent*>(event)) {
      emit dragLeaveSignal(drag_leave);
    } else if (auto* drop = dynamic_cast<QDropEvent*>(event)) {
      emit dropSignal(drop);
    }
  };

  const bool use_opengl = QSettings().value("Preferences::use_opengl", true).toBool();

  // TODO(theme): QwtPlotCanvas uses a backing-store paint path that ignores
  //   QSS background rules, so the canvas needs a solid palette colour here.
  //   Currently baked to the light-theme ${dark_background} value (#F5F5F5);
  //   should be wired to react on themeChanged. See resources/visual_guidelines.md §5.
  const QColor canvas_bg(0xf5, 0xf5, 0xf5);

  QWidget* abs_canvas = nullptr;
  if (use_opengl) {
    auto* canvas = new QwtPlotOpenGLCanvas();
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(canvas_bg);
    abs_canvas = canvas;
  } else {
    auto* canvas = new QwtPlotCanvas();
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(canvas_bg);
    canvas->setPaintAttribute(QwtPlotCanvas::BackingStore, true);
    abs_canvas = canvas;
  }
  abs_canvas->setObjectName("qwtCanvas");

  plot_ = new QwtPlotPimpl(this, abs_canvas, on_view_resized, on_event);

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(plot_);

  plot_->setMinimumSize(100, 100);
  plot_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  plot_->canvas()->setMouseTracking(true);
  // Opaque background: a transparent canvas forces a per-paint alpha-composite of the whole
  // canvas (slow in software raster) instead of a fast opaque blit. canvas_bg matches the palette set above.
  plot_->setCanvasBackground(canvas_bg);
  plot_->setAxisAutoScale(QwtPlot::yLeft, true);
  plot_->setAxisAutoScale(QwtPlot::xBottom, true);
  plot_->axisScaleEngine(QwtPlot::xBottom)->setAttribute(QwtScaleEngine::Floating, true);
  plot_->plotLayout()->setAlignCanvasToScales(true);
  plot_->setAxisScale(QwtPlot::xBottom, 0.0, 1.0);
  plot_->setAxisScale(QwtPlot::yLeft, 0.0, 1.0);
}

PlotWidgetBase::~PlotWidgetBase() {
  delete plot_;
  plot_ = nullptr;
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::addCurve(
    const QString& name, QwtSeriesData<QPointF>* series, QColor color, const QString& display_name) {
  if (series == nullptr || curveFromTitle(name) != nullptr) {
    delete series;
    return nullptr;
  }

  auto* curve = new QwtPlotCurve(display_name.isEmpty() ? name : display_name);
  curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
  curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
  curve->setData(series);
  curve->setPen(color == Qt::transparent ? nextColor() : color);
  setStyle(curve, curveStyle());
  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);
  curve->attach(qwtPlot());

  auto* marker = new QwtPlotMarker;
  marker->attach(qwtPlot());
  marker->setVisible(false);
  marker->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, Qt::red, QPen(Qt::black), QSize(8, 8)));

  plot_->curve_list.push_back(CurveInfo{.source_name = name, .curve = curve, .marker = marker});
  emit curveListChanged();
  return &plot_->curve_list.back();
}

void PlotWidgetBase::removeCurve(const QString& title) {
  auto it = std::find_if(plot_->curve_list.begin(), plot_->curve_list.end(), [&title](const CurveInfo& info) {
    return info.curve != nullptr && (info.curve->title().text() == title || info.source_name == title);
  });
  if (it == plot_->curve_list.end()) {
    return;
  }

  it->curve->detach();
  delete it->curve;
  it->marker->detach();
  delete it->marker;
  plot_->curve_list.erase(it);
  emit curveListChanged();
}

const std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList() const noexcept {
  return plot_->curve_list;
}

std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList() noexcept {
  return plot_->curve_list;
}

bool PlotWidgetBase::isEmpty() const noexcept {
  return plot_->curve_list.empty();
}

std::map<QString, QColor> PlotWidgetBase::curveColors() const {
  std::map<QString, QColor> colors;
  for (const auto& info : plot_->curve_list) {
    colors.insert({info.curve->title().text(), info.curve->pen().color()});
  }
  return colors;
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::curveFromTitle(const QString& title) {
  for (auto& info : plot_->curve_list) {
    if (info.curve->title().text() == title || info.source_name == title) {
      return &info;
    }
  }
  return nullptr;
}

void PlotWidgetBase::resetZoom() {
  updateMaximumZoomArea();
  applyRectToAxes(maxZoomRect());
  replot();
}

void PlotWidgetBase::applyRectToAxes(const QRectF& rect) {
  plot_->setAxisScale(QwtPlot::yLeft, std::min(rect.bottom(), rect.top()), std::max(rect.bottom(), rect.top()));
  plot_->setAxisScale(QwtPlot::xBottom, std::min(rect.left(), rect.right()), std::max(rect.left(), rect.right()));
  plot_->updateAxes();
}

Range<double> PlotWidgetBase::getVisualizationRangeX() const {
  double left = std::numeric_limits<double>::max();
  double right = std::numeric_limits<double>::lowest();

  for (const auto& info : curveList()) {
    if (info.curve == nullptr || !info.curve->isVisible() || info.curve->data() == nullptr) {
      continue;
    }
    const QRectF rect = info.curve->data()->boundingRect();
    if (!isUsableRect(rect)) {
      continue;
    }
    left = std::min(left, rect.left());
    right = std::max(right, rect.right());
  }

  if (left > right) {
    left = 0.0;
    right = 0.0;
  }

  if (isXYPlot() && std::abs(right - left) > std::numeric_limits<double>::epsilon()) {
    const double margin = (right - left) * 0.025;
    left -= margin;
    right += margin;
  }
  return Range<double>{.min = left, .max = right};
}

Range<double> PlotWidgetBase::getVisualizationRangeY(Range<double> range_x) const {
  double bottom = std::numeric_limits<double>::max();
  double top = std::numeric_limits<double>::lowest();

  for (const auto& info : curveList()) {
    if (info.curve == nullptr || !info.curve->isVisible() || info.curve->data() == nullptr) {
      continue;
    }

    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      const auto y_range = adapter->visibleYRange(range_x.min, range_x.max);
      if (y_range.has_value()) {
        bottom = std::min(bottom, y_range->first);
        top = std::max(top, y_range->second);
      }
      continue;
    }

    const QRectF rect = info.curve->data()->boundingRect();
    if (!isUsableRect(rect)) {
      continue;
    }
    bottom = std::min(bottom, rect.top());
    top = std::max(top, rect.bottom());
  }

  if (bottom > top) {
    bottom = -1.0;
    top = 1.0;
  }

  const double margin = (top - bottom) * 0.025;
  return Range<double>{.min = bottom - margin, .max = top + margin};
}

void PlotWidgetBase::setModeXY(bool enable) {
  xy_mode_ = enable;
}

bool PlotWidgetBase::isXYPlot() const noexcept {
  return xy_mode_;
}

void PlotWidgetBase::setLegendSize(int size) {
  QFont font = plot_->legend->font();
  font.setPointSize(size);
  plot_->legend->setFont(font);
  replot();
}

void PlotWidgetBase::setLegendAlignment(Qt::Alignment alignment) {
  plot_->legend->setAlignmentInCanvas(alignment);
}

void PlotWidgetBase::setLegendVisible(bool visible) {
  plot_->legend->setVisible(visible);
  replot();
}

bool PlotWidgetBase::legendVisible() const noexcept {
  return plot_->legend->isVisible();
}

void PlotWidgetBase::setGridVisible(bool visible) {
  plot_->grid->enableX(visible);
  plot_->grid->enableXMin(visible);
  plot_->grid->enableY(visible);
  plot_->grid->enableYMin(visible);
  replot();
}

bool PlotWidgetBase::gridVisible() const noexcept {
  return plot_->grid->xEnabled();
}

void PlotWidgetBase::setZoomEnabled(bool enabled) {
  plot_->zoom_enabled = enabled;
  plot_->zoomer->setEnabled(enabled);
  plot_->magnifier->setEnabled(enabled);
  plot_->panner1->setEnabled(enabled);
  plot_->panner2->setEnabled(enabled);
}

bool PlotWidgetBase::isZoomEnabled() const noexcept {
  return plot_->zoom_enabled;
}

void PlotWidgetBase::setSwapZoomPan(bool swapped) {
  if (swapped) {
    plot_->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::ControlModifier);
    plot_->panner1->setMouseButton(Qt::LeftButton, Qt::NoModifier);
  } else {
    plot_->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);
    plot_->panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
  }
}

QRectF PlotWidgetBase::currentBoundingRect() const {
  return plot_->canvasBoundingRect();
}

QRectF PlotWidgetBase::maxZoomRect() const noexcept {
  return max_zoom_rect_;
}

bool PlotWidgetBase::keepRatioXY() const noexcept {
  return keep_aspect_ratio_;
}

void PlotWidgetBase::setKeepRatioXY(bool active) {
  keep_aspect_ratio_ = active;
  plot_->zoomer->keepAspectRatio(isXYPlot() && active);
  if (!isXYPlot()) {
    return;
  }
  // Reshape current view; otherwise the toggle only takes effect on the next
  // drag-zoom. OFF re-fits to data bounds (loses zoom by design).
  if (active) {
    applyRectKeepingRatio(currentBoundingRect());
    replot();
  } else {
    // resetZoom() refreshes max bounds first: data may have changed since the
    // last reset, so the cached max_zoom_rect_ could be stale.
    resetZoom();
  }
}

void PlotWidgetBase::applyRectKeepingRatio(QRectF rect) {
  if (isXYPlot() && keep_aspect_ratio_) {
    plot_->zoomer->applyKeepAspectRatio(rect);
  }
  applyRectToAxes(rect);
}

void PlotWidgetBase::setAcceptDrops(bool accept) {
  plot_->setAcceptDrops(accept);
  plot_->canvas()->setAcceptDrops(accept);
}

void PlotWidgetBase::overrideCurvesStyle(std::optional<CurveStyle> style) {
  if (plot_->overridden_curve_style == style) {
    return;
  }
  plot_->overridden_curve_style = style;
  updateCurvesStyle();
}

std::optional<PlotWidgetBase::CurveStyle> PlotWidgetBase::overriddenCurvesStyle() const noexcept {
  return plot_->overridden_curve_style;
}

void PlotWidgetBase::setDefaultStyle(CurveStyle default_style) {
  plot_->default_curve_style = default_style;
  updateCurvesStyle();
}

PlotWidgetBase::CurveStyle PlotWidgetBase::defaultCurveStyle() const noexcept {
  return plot_->default_curve_style;
}

PlotWidgetBase::CurveStyle PlotWidgetBase::curveStyle() const noexcept {
  return plot_->overridden_curve_style.value_or(plot_->default_curve_style);
}

void PlotWidgetBase::updateCurvesStyle() {
  for (auto& info : plot_->curve_list) {
    setStyle(info.curve, curveStyle());
  }
  replot();
}

void PlotWidgetBase::setLineWidth(LineWidth width) {
  line_width_ = width;
  for (auto& info : plot_->curve_list) {
    info.curve->setPen(info.curve->pen().color(), lineWidthValue(width));
  }
  replot();
}

void PlotWidgetBase::replot() {
  if (plot_->zoomer != nullptr) {
    plot_->zoomer->setZoomBase(false);
  }
  plot_->replot();
}

void PlotWidgetBase::removeAllCurves() {
  for (auto& info : plot_->curve_list) {
    info.curve->detach();
    delete info.curve;
    info.marker->detach();
    delete info.marker;
  }
  plot_->curve_list.clear();
  emit curveListChanged();
  replot();
}

void PlotWidgetBase::setStyle(QwtPlotCurve* curve, CurveStyle style) {
  const double width = style == kDots ? dotWidthValue(lineWidth()) : lineWidthValue(lineWidth());
  curve->setPen(curve->pen().color(), width);

  // Qwt's QwtPlotCurve::LinesAndDots style on its own does not draw visible
  // dots at the pen widths we use (1.4-4.2 px); attach an explicit symbol so
  // each sample is rendered as a small filled circle. Cleared for plain Lines.
  switch (style) {
    case kLines:
      curve->setStyle(QwtPlotCurve::Lines);
      curve->setSymbol(nullptr);
      break;
    case kLinesAndDots: {
      curve->setStyle(QwtPlotCurve::Lines);
      const QColor color = curve->pen().color();
      const int dot_size = static_cast<int>(std::round(dotWidthValue(lineWidth())));
      curve->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, color, QPen(color), QSize(dot_size, dot_size)));
      break;
    }
    case kDots:
      curve->setStyle(QwtPlotCurve::Dots);
      curve->setSymbol(nullptr);
      break;
    case kSticks:
      curve->setStyle(QwtPlotCurve::Sticks);
      curve->setSymbol(nullptr);
      break;
    case kSteps:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, false);
      curve->setSymbol(nullptr);
      break;
    case kStepsInverted:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, true);
      curve->setSymbol(nullptr);
      break;
  }
}

QColor PlotWidgetBase::nextColor() {
  return colorFromIndex(next_color_index_++);
}

QColor PlotWidgetBase::paletteColor(int index) {
  return colorFromIndex(index);
}

QwtPlot* PlotWidgetBase::qwtPlot() {
  return plot_;
}

void PlotWidgetBase::installHoverFilter(QObject* filter) {
  if (filter == nullptr || plot_ == nullptr) {
    return;
  }
  if (QWidget* canvas = plot_->canvas(); canvas != nullptr) {
    canvas->installEventFilter(filter);
  }
  for (int axis : {QwtAxis::YLeft, QwtAxis::YRight, QwtAxis::XBottom, QwtAxis::XTop}) {
    if (plot_->isAxisVisible(axis)) {
      if (auto* widget = plot_->axisWidget(axis); widget != nullptr) {
        widget->installEventFilter(filter);
      }
    }
  }
}

const QwtPlot* PlotWidgetBase::qwtPlot() const {
  return plot_;
}

PlotLegend* PlotWidgetBase::legend() {
  return plot_->legend;
}

PlotZoomer* PlotWidgetBase::zoomer() {
  return plot_->zoomer;
}

PlotMagnifier* PlotWidgetBase::magnifier() {
  return plot_->magnifier;
}

PlotPanner* PlotWidgetBase::panner1() {
  return plot_->panner1;
}

PlotPanner* PlotWidgetBase::panner2() {
  return plot_->panner2;
}

void PlotWidgetBase::updateMaximumZoomArea() {
  QRectF max_rect;
  const Range<double> range_x = getVisualizationRangeX();
  max_rect.setLeft(range_x.min);
  max_rect.setRight(range_x.max);

  const Range<double> range_y = getVisualizationRangeY(range_x);
  max_rect.setBottom(range_y.min);
  max_rect.setTop(range_y.max);

  if (isXYPlot() && keep_aspect_ratio_) {
    const QRectF canvas_rect = plot_->canvas()->contentsRect();
    const double canvas_ratio = std::abs(canvas_rect.width() / canvas_rect.height());
    const double data_ratio = std::abs(max_rect.width() / max_rect.height());
    if (data_ratio < canvas_ratio) {
      const double new_width = std::abs(max_rect.height() * canvas_ratio);
      const double increment = new_width - max_rect.width();
      max_rect.setWidth(new_width);
      max_rect.moveLeft(max_rect.left() - 0.5 * increment);
    } else {
      const double new_height = -(max_rect.width() / canvas_ratio);
      const double increment = std::abs(new_height - max_rect.height());
      max_rect.setHeight(new_height);
      max_rect.moveTop(max_rect.top() + 0.5 * increment);
    }
  }

  magnifier()->setAxisLimits(QwtPlot::xBottom, max_rect.left(), max_rect.right());
  magnifier()->setAxisLimits(QwtPlot::yLeft, max_rect.bottom(), max_rect.top());
  zoomer()->keepAspectRatio(isXYPlot() && keep_aspect_ratio_);
  max_zoom_rect_ = max_rect;
}

bool PlotWidgetBase::eventFilter(QObject* obj, QEvent* event) {
  if (event->type() == QEvent::Destroy) {
    return false;
  }

  QwtScaleWidget* bottom_axis = plot_->axisWidget(QwtPlot::xBottom);
  QwtScaleWidget* left_axis = plot_->axisWidget(QwtPlot::yLeft);

  if ((obj == bottom_axis || obj == left_axis) && !(isXYPlot() && keepRatioXY()) && event->type() == QEvent::Wheel) {
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    magnifier()->setDefaultMode(obj == bottom_axis ? PlotMagnifier::kXAxis : PlotMagnifier::kYAxis);
    magnifier()->widgetWheelEvent(wheel_event);
  }

  if (obj != plot_->canvas()) {
    return false;
  }

  if (event->type() == QEvent::DragEnter) {
    emit dragEnterSignal(static_cast<QDragEnterEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::DragLeave) {
    emit dragLeaveSignal(static_cast<QDragLeaveEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::Drop) {
    emit dropSignal(static_cast<QDropEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::Wheel) {
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    magnifier()->setDefaultMode(PlotMagnifier::kBothAxes);

    const bool ctrl_modifier = wheel_event->modifiers() == Qt::ControlModifier;
    const QRectF legend_rect = legend()->geometry(plot_->canvas()->rect());
    if (ctrl_modifier && legend()->isVisible() && legend_rect.contains(wheel_event->position())) {
      const int previous_size = legend()->font().pointSize();
      int new_size = previous_size;
      if (wheel_event->angleDelta().y() > 0) {
        new_size = std::min(13, previous_size + 1);
      } else if (wheel_event->angleDelta().y() < 0) {
        new_size = std::max(7, previous_size - 1);
      }
      if (new_size != previous_size) {
        setLegendSize(new_size);
        emit legendSizeChanged(new_size);
      }
      return true;
    }
    return false;
  }

  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() == Qt::LeftButton && mouse_event->modifiers() == Qt::NoModifier) {
      const QwtPlotItem* clicked_item = legend()->processMousePressEvent(mouse_event);
      if (clicked_item == nullptr) {
        return false;
      }
      for (auto& info : curveList()) {
        if (clicked_item == info.curve) {
          info.curve->setVisible(!info.curve->isVisible());
          resetZoom();
          replot();
          return true;
        }
      }
    }
  }

  return false;
}

}  // namespace PJ
