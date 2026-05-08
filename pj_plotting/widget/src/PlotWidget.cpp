#include "pj_plotting/PlotWidget.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_scale_map.h>
#include <qwt_text.h>

#include <QColorDialog>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QIODevice>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPen>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString curveKey(const QString& source_name, const QString& x_name = {}, const QString& y_name = {}) {
  if (!x_name.isEmpty() || !y_name.isEmpty()) {
    return QStringLiteral("xy:") + x_name + QStringLiteral("\n") + y_name;
  }
  return QStringLiteral("ts:") + source_name;
}

}  // namespace

PlotWidget::PlotWidget(SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : PlotWidgetBase(parent), session_(session), catalog_(catalog) {
  state_id_ = newStateId();
  setAcceptDrops(true);
  tracker_ = new CurveTracker(qwtPlot(), QColor(Qt::red));
  connect(this, &PlotWidgetBase::viewResized, this, &PlotWidget::onExternallyResized);
  connect(this, &PlotWidgetBase::curveListChanged, this, [this]() { updateMaximumZoomArea(); });
  connect(this, &PlotWidgetBase::dragEnterSignal, this, &PlotWidget::onDragEnterEvent);
  connect(this, &PlotWidgetBase::dragLeaveSignal, this, &PlotWidget::onDragLeaveEvent);
  connect(this, &PlotWidgetBase::dropSignal, this, &PlotWidget::onDropEvent);
  buildActions();
  reconnectDataSignals();
}

PlotWidget::~PlotWidget() {
  delete tracker_;
  delete action_split_horizontal_;
  delete action_split_vertical_;
  delete action_remove_all_curves_;
  delete action_zoom_out_;
  delete action_zoom_out_horizontal_;
  delete action_zoom_out_vertical_;
}

void PlotWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  if (session_ == session && catalog_ == catalog) {
    return;
  }
  session_ = session;
  catalog_ = catalog;
  reconnectDataSignals();
}

PlotWidget::CurveInfo* PlotWidget::addCurve(const QString& name, QColor color) {
  if (session_ == nullptr || catalog_ == nullptr) {
    return nullptr;
  }

  const std::optional<CurveDescriptor> descriptor = catalog_->curveDescriptor(name);
  if (!descriptor.has_value()) {
    return nullptr;
  }

  auto* adapter = new DatastoreCurveAdapter(session_, *descriptor);
  CurveInfo* info = PlotWidgetBase::addCurve(name, adapter, color);
  if (info == nullptr) {
    return nullptr;
  }
  if (tracker_ != nullptr) {
    tracker_->setEnabled(tracker_enabled_);
  }
  updateMaximumZoomArea();
  replot();
  return info;
}

PlotWidget::CurveInfo* PlotWidget::addCurveXY(const QString& x_name, const QString& y_name, QColor color) {
  if (session_ == nullptr || catalog_ == nullptr) {
    return nullptr;
  }

  const std::optional<CurveDescriptor> x_descriptor = catalog_->curveDescriptor(x_name);
  const std::optional<CurveDescriptor> y_descriptor = catalog_->curveDescriptor(y_name);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  const QString title = tr("%1 vs %2").arg(y_name, x_name);
  auto* series = new PointSeriesXY(session_, *x_descriptor, *y_descriptor);
  CurveInfo* info = PlotWidgetBase::addCurve(title, series, color);
  if (info == nullptr) {
    return nullptr;
  }
  if (tracker_ != nullptr) {
    tracker_->setEnabled(false);
  }
  info->curve->setStyle(QwtPlotCurve::Dots);
  info->curve->setPen(info->curve->pen().color(), dotWidthValue(lineWidth()));
  updateMaximumZoomArea();
  replot();
  return info;
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal) {
  setAxisScale(QwtPlot::yLeft, rect.bottom(), rect.top());
  setAxisScale(QwtPlot::xBottom, rect.left(), rect.right());
  qwtPlot()->updateAxes();

  if (emit_signal) {
    if (isXYPlot()) {
      emit undoableChange();
    } else {
      emit rectChanged(this, rect);
    }
  }
}

bool PlotWidget::isZoomLinkEnabled() const noexcept {
  return true;
}

void PlotWidget::setTrackerEnabled(bool enabled) {
  tracker_enabled_ = enabled;
  if (tracker_ != nullptr) {
    tracker_->setEnabled(enabled && !isXYPlot());
  }
  replot();
}

bool PlotWidget::trackerEnabled() const noexcept {
  return tracker_enabled_;
}

QString PlotWidget::stateId() const {
  return state_id_;
}

void PlotWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

QDomElement PlotWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement plot_element = doc.createElement(QStringLiteral("plot"));
  plot_element.setAttribute(QStringLiteral("id"), state_id_);
  plot_element.setAttribute(
      QStringLiteral("mode"), isXYPlot() ? QStringLiteral("XYPlot") : QStringLiteral("TimeSeries"));
  plot_element.setAttribute(QStringLiteral("line_width"), lineWidthToString(lineWidth()));
  plot_element.setAttribute(QStringLiteral("title"), qwtPlot()->title().text());
  plot_element.setAttribute(
      QStringLiteral("tracker_enabled"), tracker_enabled_ ? QStringLiteral("true") : QStringLiteral("false"));

  QDomElement range_element = doc.createElement(QStringLiteral("range"));
  const QRectF rect = currentBoundingRect();
  range_element.setAttribute(QStringLiteral("bottom"), QString::number(rect.bottom(), 'f', 6));
  range_element.setAttribute(QStringLiteral("top"), QString::number(rect.top(), 'f', 6));
  range_element.setAttribute(QStringLiteral("left"), QString::number(rect.left(), 'f', 6));
  range_element.setAttribute(QStringLiteral("right"), QString::number(rect.right(), 'f', 6));
  plot_element.appendChild(range_element);

  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QDomElement curve_element = doc.createElement(QStringLiteral("curve"));
    curve_element.setAttribute(QStringLiteral("name"), info.source_name);
    curve_element.setAttribute(QStringLiteral("color"), info.curve->pen().color().name());
    curve_element.setAttribute(QStringLiteral("line_width"), QString::number(info.curve->pen().widthF(), 'f', 2));
    curve_element.setAttribute(QStringLiteral("style"), curveStyleToString(qwtStyleToCurveStyle(info.curve)));
    curve_element.setAttribute(
        QStringLiteral("visible"), info.curve->isVisible() ? QStringLiteral("true") : QStringLiteral("false"));
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      curve_element.setAttribute(QStringLiteral("curve_x"), xy_series->xSource().name);
      curve_element.setAttribute(QStringLiteral("curve_y"), xy_series->ySource().name);
    }
    plot_element.appendChild(curve_element);
  }

  return plot_element;
}

bool PlotWidget::xmlLoadState(const QDomElement& plot_element, bool autozoom) {
  if (plot_element.isNull() || plot_element.tagName() != QStringLiteral("plot")) {
    return false;
  }

  setStateId(plot_element.attribute(QStringLiteral("id")));
  setModeXY(plot_element.attribute(QStringLiteral("mode")) == QStringLiteral("XYPlot"));
  setLineWidth(lineWidthFromString(plot_element.attribute(QStringLiteral("line_width"), QStringLiteral("1.0"))));
  setTrackerEnabled(
      plot_element.attribute(QStringLiteral("tracker_enabled"), QStringLiteral("true")) == QStringLiteral("true"));
  qwtPlot()->setTitle(plot_element.attribute(QStringLiteral("title")));

  std::set<QString> desired_keys;
  for (QDomElement curve_element = plot_element.firstChildElement(QStringLiteral("curve")); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(QStringLiteral("curve"))) {
    if (isXYPlot() && curve_element.hasAttribute(QStringLiteral("curve_x")) &&
        curve_element.hasAttribute(QStringLiteral("curve_y"))) {
      desired_keys.insert(curveKey(
          curve_element.attribute(QStringLiteral("name")), curve_element.attribute(QStringLiteral("curve_x")),
          curve_element.attribute(QStringLiteral("curve_y"))));
    } else {
      desired_keys.insert(curveKey(curve_element.attribute(QStringLiteral("name"))));
    }
  }

  QStringList remove_titles;
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QString existing_key = curveKey(info.source_name);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      existing_key = curveKey(info.source_name, xy_series->xSource().name, xy_series->ySource().name);
    }
    if (desired_keys.find(existing_key) == desired_keys.end()) {
      remove_titles.push_back(info.curve->title().text());
    }
  }
  for (const QString& title : remove_titles) {
    removeCurve(title);
  }

  for (QDomElement curve_element = plot_element.firstChildElement(QStringLiteral("curve")); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(QStringLiteral("curve"))) {
    const QColor color(curve_element.attribute(QStringLiteral("color")));
    CurveInfo* loaded_curve = nullptr;
    if (isXYPlot() && curve_element.hasAttribute(QStringLiteral("curve_x")) &&
        curve_element.hasAttribute(QStringLiteral("curve_y"))) {
      const QString x_name = curve_element.attribute(QStringLiteral("curve_x"));
      const QString y_name = curve_element.attribute(QStringLiteral("curve_y"));
      const QString source_name = curve_element.attribute(QStringLiteral("name"));
      for (CurveInfo& info : curveList()) {
        if (auto* xy_series = info.curve != nullptr ? dynamic_cast<PointSeriesXY*>(info.curve->data()) : nullptr) {
          if (curveKey(info.source_name, xy_series->xSource().name, xy_series->ySource().name) ==
              curveKey(source_name, x_name, y_name)) {
            loaded_curve = &info;
            break;
          }
        }
      }
      if (loaded_curve == nullptr) {
        loaded_curve = addCurveXY(x_name, y_name, color.isValid() ? color : Qt::transparent);
      }
    } else {
      const QString curve_name = curve_element.attribute(QStringLiteral("name"));
      loaded_curve = curveFromTitle(curve_name);
      if (loaded_curve == nullptr) {
        loaded_curve = addCurve(curve_name, color.isValid() ? color : Qt::transparent);
      }
    }
    if (loaded_curve != nullptr && loaded_curve->curve != nullptr && color.isValid()) {
      loaded_curve->curve->setPen(color, loaded_curve->curve->pen().widthF());
    }
    if (loaded_curve != nullptr && curve_element.hasAttribute(QStringLiteral("line_width"))) {
      bool ok = false;
      const double width = curve_element.attribute(QStringLiteral("line_width")).toDouble(&ok);
      if (ok) {
        loaded_curve->curve->setPen(loaded_curve->curve->pen().color(), width);
      }
    }
    if (loaded_curve != nullptr && curve_element.hasAttribute(QStringLiteral("style"))) {
      // Apply per-curve style after the per-curve width above so the style
      // toggle path (which leaves the pen alone) does not undo the width.
      setCurveStyle(loaded_curve->source_name, curveStyleFromString(curve_element.attribute(QStringLiteral("style"))));
    }
    if (loaded_curve != nullptr) {
      const QString visible_attr = curve_element.attribute(QStringLiteral("visible"), QStringLiteral("true"));
      loaded_curve->curve->setVisible(visible_attr == QStringLiteral("true"));
    }
  }

  const QDomElement range_element = plot_element.firstChildElement(QStringLiteral("range"));
  if (!range_element.isNull() && autozoom) {
    QRectF rect;
    rect.setBottom(range_element.attribute(QStringLiteral("bottom")).toDouble());
    rect.setTop(range_element.attribute(QStringLiteral("top")).toDouble());
    rect.setLeft(range_element.attribute(QStringLiteral("left")).toDouble());
    rect.setRight(range_element.attribute(QStringLiteral("right")).toDouble());
    setZoomRectangle(rect, false);
  } else {
    zoomOut(false);
  }
  replot();
  return true;
}

void PlotWidget::zoomOut(bool emit_signal) {
  if (curveList().empty()) {
    setZoomRectangle(QRectF(0, 1, 1, -1), false);
    return;
  }

  updateMaximumZoomArea();
  setZoomRectangle(maxZoomRect(), emit_signal);
  replot();
}

void PlotWidget::onZoomOutHorizontalTriggered(bool emit_signal) {
  updateMaximumZoomArea();
  QRectF rect = currentBoundingRect();
  const Range<double> range_x = getVisualizationRangeX();
  rect.setLeft(range_x.min);
  rect.setRight(range_x.max);
  setZoomRectangle(rect, emit_signal);
  replot();
}

void PlotWidget::onZoomOutVerticalTriggered(bool emit_signal) {
  updateMaximumZoomArea();
  QRectF rect = currentBoundingRect();
  const Range<double> range_y = getVisualizationRangeY(Range<double>{.min = rect.left(), .max = rect.right()});
  rect.setBottom(range_y.min);
  rect.setTop(range_y.max);
  setZoomRectangle(rect, emit_signal);
  replot();
}

void PlotWidget::setTrackerPosition(double display_time_sec) {
  if (tracker_ == nullptr) {
    return;
  }
  tracker_->setEnabled(tracker_enabled_ && !isXYPlot());
  if (isXYPlot()) {
    return;
  }
  tracker_->setPosition(QPointF(display_time_sec, 0.0));
  replot();
}

void PlotWidget::onChangeCurveColor(const QString& curve_name, QColor new_color) {
  for (auto& info : curveList()) {
    if (info.curve->title().text() == curve_name) {
      info.curve->setPen(new_color, info.curve->pen().widthF());
      replot();
      return;
    }
  }
}

void PlotWidget::setCurveLineWidth(const QString& curve_name, double width) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->curve->setPen(info->curve->pen().color(), width);
  replot();
  emit undoableChange();
}

void PlotWidget::setCurveStyle(const QString& curve_name, CurveStyle style) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  // Mirror PlotWidgetBase::setStyle()'s style-to-Qwt mapping (Steps + Inverted
  // attribute), but skip the pen-width assignment so per-curve width set via
  // setCurveLineWidth() survives a style toggle.
  switch (style) {
    case kLines:
      info->curve->setStyle(QwtPlotCurve::Lines);
      break;
    case kLinesAndDots:
      info->curve->setStyle(QwtPlotCurve::LinesAndDots);
      break;
    case kDots:
      info->curve->setStyle(QwtPlotCurve::Dots);
      break;
    case kSticks:
      info->curve->setStyle(QwtPlotCurve::Sticks);
      break;
    case kSteps:
      info->curve->setStyle(QwtPlotCurve::Steps);
      info->curve->setCurveAttribute(QwtPlotCurve::Inverted, false);
      break;
    case kStepsInverted:
      info->curve->setStyle(QwtPlotCurve::Steps);
      info->curve->setCurveAttribute(QwtPlotCurve::Inverted, true);
      break;
  }
  replot();
  emit undoableChange();
}

void PlotWidget::setCurveVisible(const QString& curve_name, bool visible) {
  CurveInfo* info = curveFromTitle(curve_name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  info->curve->setVisible(visible);
  replot();
  emit undoableChange();
}

void PlotWidget::removeAllCurves() {
  PlotWidgetBase::removeAllCurves();
  setModeXY(false);
  if (tracker_ != nullptr) {
    tracker_->setEnabled(tracker_enabled_);
    tracker_->redraw();
  }
}

bool PlotWidget::eventFilter(QObject* obj, QEvent* event) {
  if (PlotWidgetBase::eventFilter(obj, event)) {
    return true;
  }
  if (event->type() == QEvent::Destroy || obj != qwtPlot()->canvas()) {
    return false;
  }

  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier && !isXYPlot()) {
      const QwtScaleMap x_map = qwtPlot()->canvasMap(QwtPlot::xBottom);
      const QwtScaleMap y_map = qwtPlot()->canvasMap(QwtPlot::yLeft);
      emit trackerMoved(
          QPointF(x_map.invTransform(mouse_event->pos().x()), y_map.invTransform(mouse_event->pos().y())));
      return true;
    }
    if (mouse_event->button() == Qt::RightButton && mouse_event->modifiers() == Qt::NoModifier) {
      canvasContextMenuTriggered(mouse_event->pos());
      return true;
    }
  }
  if (event->type() == QEvent::MouseMove) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->buttons() == Qt::LeftButton && mouse_event->modifiers() == Qt::ShiftModifier && !isXYPlot()) {
      const QwtScaleMap x_map = qwtPlot()->canvasMap(QwtPlot::xBottom);
      const QwtScaleMap y_map = qwtPlot()->canvasMap(QwtPlot::yLeft);
      emit trackerMoved(
          QPointF(x_map.invTransform(mouse_event->pos().x()), y_map.invTransform(mouse_event->pos().y())));
      return true;
    }
  }
  return false;
}

void PlotWidget::onExternallyResized(const QRectF& rect) {
  if (isXYPlot() || !isZoomLinkEnabled()) {
    return;
  }
  emit rectChanged(this, rect);
}

void PlotWidget::onDragEnterEvent(QDragEnterEvent* event) {
  dragging_ = {};
  if (catalog_ == nullptr || event == nullptr || event->mimeData() == nullptr) {
    return;
  }

  const QMimeData* mime_data = event->mimeData();
  if (mime_data->hasFormat(QStringLiteral("curveslist/add_curve"))) {
    const QStringList curves = decodeCurveDrop(mime_data, QStringLiteral("curveslist/add_curve"));
    if (!curves.empty() && allCurvesKnown(curves) && !isXYPlot()) {
      dragging_.mode = DragMode::kCurves;
      dragging_.curves = curves;
      event->acceptProposedAction();
    }
    return;
  }

  if (mime_data->hasFormat(QStringLiteral("curveslist/new_XY_axis"))) {
    const QStringList curves = decodeCurveDrop(mime_data, QStringLiteral("curveslist/new_XY_axis"));
    if (curves.size() == 2 && allCurvesKnown(curves) && curveList().empty()) {
      dragging_.mode = DragMode::kNewXY;
      dragging_.curves = curves;
      event->acceptProposedAction();
    }
  }
}

void PlotWidget::onDragLeaveEvent(QDragLeaveEvent* /*event*/) {
  dragging_ = {};
}

void PlotWidget::onDropEvent(QDropEvent* event) {
  if (event == nullptr || dragging_.mode == DragMode::kNone) {
    return;
  }

  const bool was_empty = curveList().empty();
  bool curves_changed = false;
  if (dragging_.mode == DragMode::kCurves) {
    if (isXYPlot()) {
      emit statusMessageRequested(tr("Timeseries curves can not be dropped on an XY plot."));
    } else {
      setModeXY(false);
      for (const QString& curve_name : dragging_.curves) {
        curves_changed = addCurve(curve_name) != nullptr || curves_changed;
      }
    }
  } else if (dragging_.mode == DragMode::kNewXY && dragging_.curves.size() == 2) {
    if (!curveList().empty()) {
      emit statusMessageRequested(tr("Create XY plots by dropping two curves on an empty plot."));
    } else {
      setModeXY(true);
      curves_changed = addCurveXY(dragging_.curves[0], dragging_.curves[1]) != nullptr;
    }
  }

  if (curves_changed) {
    event->acceptProposedAction();
    emit curvesDropped();
    if (was_empty) {
      zoomOut(true);
    } else {
      replot();
    }
    emit undoableChange();
  }
  dragging_ = {};
}

void PlotWidget::buildActions() {
  action_split_horizontal_ = new QAction(tr("&Split Horizontally"), this);
  connect(action_split_horizontal_, &QAction::triggered, this, &PlotWidget::splitHorizontal);

  action_split_vertical_ = new QAction(tr("&Split Vertically"), this);
  connect(action_split_vertical_, &QAction::triggered, this, &PlotWidget::splitVertical);

  action_remove_all_curves_ = new QAction(tr("&Remove ALL curves"), this);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::removeAllCurves);
  connect(action_remove_all_curves_, &QAction::triggered, this, &PlotWidget::undoableChange);

  action_zoom_out_ = new QAction(tr("&Zoom Out"), this);
  connect(action_zoom_out_, &QAction::triggered, this, [this]() {
    zoomOut(true);
    emit undoableChange();
  });

  action_zoom_out_horizontal_ = new QAction(tr("&Zoom Out Horizontally"), this);
  connect(action_zoom_out_horizontal_, &QAction::triggered, this, [this]() {
    onZoomOutHorizontalTriggered(true);
    emit undoableChange();
  });

  action_zoom_out_vertical_ = new QAction(tr("&Zoom Out Vertically"), this);
  connect(action_zoom_out_vertical_, &QAction::triggered, this, [this]() {
    onZoomOutVerticalTriggered(true);
    emit undoableChange();
  });
}

void PlotWidget::canvasContextMenuTriggered(const QPoint& pos) {
  if (!context_menu_enabled_) {
    return;
  }

  CurveInfo* selected_curve = curveAtPosition(pos);
  QMenu menu(qwtPlot());
  if (selected_curve != nullptr) {
    menu.addAction(tr("Change color..."), this, [this, selected_curve]() {
      const QColor current_color = selected_curve->curve->pen().color();
      const QColor next_color = QColorDialog::getColor(current_color, this, tr("Pick curve color"));
      if (next_color.isValid()) {
        onChangeCurveColor(selected_curve->curve->title().text(), next_color);
        emit undoableChange();
      }
    });
    menu.addAction(tr("Remove curve"), this, [this, selected_curve]() {
      removeCurve(selected_curve->curve->title().text());
      emit undoableChange();
      replot();
    });
    menu.addSeparator();
  }
  menu.addAction(action_split_horizontal_);
  menu.addAction(action_split_vertical_);
  menu.addSeparator();
  menu.addAction(action_zoom_out_);
  menu.addAction(action_zoom_out_horizontal_);
  menu.addAction(action_zoom_out_vertical_);
  menu.addSeparator();
  menu.addAction(action_remove_all_curves_);
  action_remove_all_curves_->setEnabled(!curveList().empty());
  menu.exec(qwtPlot()->canvas()->mapToGlobal(pos));
}

PlotWidget::CurveInfo* PlotWidget::curveAtPosition(const QPoint& pos) {
  const QwtPlotItem* legend_item = legend()->itemAt(pos);
  if (legend_item != nullptr) {
    for (CurveInfo& info : curveList()) {
      if (info.curve == legend_item) {
        return &info;
      }
    }
  }

  CurveInfo* best_curve = nullptr;
  double best_distance = std::numeric_limits<double>::max();
  constexpr double kHitDistancePixels = 8.0;
  constexpr std::size_t kMaxHitTestSamples = 2000;
  for (CurveInfo& info : curveList()) {
    if (info.curve == nullptr || !info.curve->isVisible() || info.curve->dataSize() == 0) {
      continue;
    }
    const std::size_t sample_count = info.curve->dataSize();
    const std::size_t step = std::max<std::size_t>(1, sample_count / kMaxHitTestSamples);
    for (std::size_t index = 0; index < sample_count; index += step) {
      const QPointF sample = info.curve->sample(index);
      if (!std::isfinite(sample.x()) || !std::isfinite(sample.y())) {
        continue;
      }
      const double x = qwtPlot()->transform(QwtPlot::xBottom, sample.x());
      const double y = qwtPlot()->transform(QwtPlot::yLeft, sample.y());
      const double dx = x - pos.x();
      const double dy = y - pos.y();
      const double distance = std::sqrt(dx * dx + dy * dy);
      if (distance < best_distance) {
        best_distance = distance;
        best_curve = &info;
      }
    }
  }
  return best_distance <= kHitDistancePixels ? best_curve : nullptr;
}

void PlotWidget::setAxisScale(QwtAxisId axis_id, double min, double max) {
  if (min > max) {
    std::swap(min, max);
  }
  qwtPlot()->setAxisScale(axis_id, min, max);
}

QStringList PlotWidget::decodeCurveDrop(const QMimeData* mime_data, const QString& format) const {
  QStringList curves;
  if (mime_data == nullptr || !mime_data->hasFormat(format)) {
    return curves;
  }

  QByteArray encoded = mime_data->data(format);
  QDataStream stream(&encoded, QIODevice::ReadOnly);
  while (!stream.atEnd()) {
    QString curve_name;
    stream >> curve_name;
    if (!curve_name.isEmpty()) {
      curves.push_back(curve_name);
    }
  }
  return curves;
}

bool PlotWidget::allCurvesKnown(const QStringList& curves) const {
  if (catalog_ == nullptr) {
    return false;
  }
  return std::all_of(curves.begin(), curves.end(), [this](const QString& curve_name) {
    return catalog_->curveDescriptor(curve_name).has_value();
  });
}

QString PlotWidget::lineWidthToString(LineWidth width) {
  switch (width) {
    case LineWidth::kPoints1_0:
      return QStringLiteral("1.0");
    case LineWidth::kPoints1_5:
      return QStringLiteral("1.5");
    case LineWidth::kPoints2_0:
      return QStringLiteral("2.0");
    case LineWidth::kPoints3_0:
      return QStringLiteral("3.0");
  }
  return QStringLiteral("1.0");
}

LineWidth PlotWidget::lineWidthFromString(QString value) {
  if (value == QStringLiteral("1.5")) {
    return LineWidth::kPoints1_5;
  }
  if (value == QStringLiteral("2.0")) {
    return LineWidth::kPoints2_0;
  }
  if (value == QStringLiteral("3.0")) {
    return LineWidth::kPoints3_0;
  }
  return LineWidth::kPoints1_0;
}

QString PlotWidget::curveStyleToString(CurveStyle style) {
  switch (style) {
    case kLines:
      return QStringLiteral("Lines");
    case kDots:
      return QStringLiteral("Dots");
    case kLinesAndDots:
      return QStringLiteral("LinesAndDots");
    case kSticks:
      return QStringLiteral("Sticks");
    case kSteps:
      return QStringLiteral("Steps");
    case kStepsInverted:
      return QStringLiteral("StepsInverted");
  }
  return QStringLiteral("Lines");
}

PlotWidgetBase::CurveStyle PlotWidget::curveStyleFromString(QString value) {
  if (value == QStringLiteral("Dots")) {
    return kDots;
  }
  if (value == QStringLiteral("LinesAndDots")) {
    return kLinesAndDots;
  }
  if (value == QStringLiteral("Sticks")) {
    return kSticks;
  }
  if (value == QStringLiteral("Steps")) {
    return kSteps;
  }
  if (value == QStringLiteral("StepsInverted")) {
    return kStepsInverted;
  }
  return kLines;
}

PlotWidgetBase::CurveStyle PlotWidget::qwtStyleToCurveStyle(const QwtPlotCurve* curve) {
  if (curve == nullptr) {
    return kLines;
  }
  switch (curve->style()) {
    case QwtPlotCurve::Lines:
      return kLines;
    case QwtPlotCurve::Dots:
      return kDots;
    case QwtPlotCurve::LinesAndDots:
      return kLinesAndDots;
    case QwtPlotCurve::Sticks:
      return kSticks;
    case QwtPlotCurve::Steps:
      return curve->testCurveAttribute(QwtPlotCurve::Inverted) ? kStepsInverted : kSteps;
    default:
      return kLines;
  }
}

void PlotWidget::reconnectDataSignals() {
  if (topics_committed_connection_) {
    disconnect(topics_committed_connection_);
  }
  if (session_ == nullptr) {
    return;
  }

  topics_committed_connection_ =
      connect(session_, &SessionManager::topicsCommitted, this, [this](const QVector<TopicId>& ids) {
        bool changed = false;
        for (auto& info : curveList()) {
          auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data());
          if (adapter != nullptr) {
            if (std::find(ids.begin(), ids.end(), adapter->source().topic_id) != ids.end()) {
              adapter->onTopicCommitted();
              changed = true;
            }
            continue;
          }

          auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data());
          if (xy_series == nullptr) {
            continue;
          }
          if (std::find(ids.begin(), ids.end(), xy_series->xSource().topic_id) != ids.end() ||
              std::find(ids.begin(), ids.end(), xy_series->ySource().topic_id) != ids.end()) {
            xy_series->onTopicCommitted();
            changed = true;
          }
        }
        if (changed) {
          updateMaximumZoomArea();
          replot();
        }
      });
}

}  // namespace PJ
