// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotWidget.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_item.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

#include <QColorDialog>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFontDatabase>
#include <QIODevice>
#include <QIcon>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPalette>
#include <QPen>
#include <QSettings>
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
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {
namespace {

constexpr int kHoverHitRadiusPx = 40;

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString curveKey(const QString& source_name, const QString& x_name = {}, const QString& y_name = {}) {
  if (!x_name.isEmpty() || !y_name.isEmpty()) {
    return QStringLiteral("xy:") + x_name + QStringLiteral("\n") + y_name;
  }
  return QStringLiteral("ts:") + source_name;
}

void appendDisplayPath(QString& base, QString path) {
  path.replace('.', '/');
  while (path.startsWith('/')) {
    path.remove(0, 1);
  }
  if (path.isEmpty()) {
    return;
  }
  if (!base.isEmpty() && !base.endsWith('/')) {
    base += '/';
  }
  base += path;
}

QString curveDisplayName(const CurveDescriptor& descriptor) {
  QString name;
  appendDisplayPath(name, descriptor.topic_name);
  appendDisplayPath(name, descriptor.field_name);
  return name.isEmpty() ? descriptor.name : name;
}

// The session-scoped curve-color registry (issue #68), reached through the
// SessionManager the widget already holds. Returns nullptr when no session is
// wired (e.g. a service-less PlotWidget in a unit test), in which case the base
// class's per-widget nextColor() rotation is the fallback.
CurveColorRegistry* colorRegistryOf(SessionManager* session) {
  return session != nullptr ? &session->curveColorRegistry() : nullptr;
}

}  // namespace

PlotWidget::PlotWidget(SessionManager* session, CatalogModel* catalog, QWidget* parent)
    : PlotWidgetBase(parent), session_(session), catalog_(catalog) {
  state_id_ = newStateId();
  setAcceptDrops(true);
  tracker_ = new CurveTracker(qwtPlot(), QColor(Qt::red));
  reference_tracker_ = new CurveTracker(qwtPlot(), QColor(Qt::blue));
  reference_tracker_->setParameter(CurveTracker::kLineOnly);
  reference_tracker_->setEnabled(false);

  // Mouse-hover inspector. Shows a snap-to-curve dot + value tooltip wherever
  // the mouse points, gated by show_points_. Independent from the playback
  // tracker (tracker_): the red playback line always shows the current
  // playback time and is not affected by this toggle.
  show_point_marker_ = new QwtPlotMarker();
  show_point_marker_->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, QColor(Qt::yellow), QPen(Qt::black), QSize(8, 8)));
  show_point_marker_->setVisible(false);
  show_point_marker_->attach(qwtPlot());

  show_point_text_ = new QwtPlotMarker();
  show_point_text_->setVisible(false);
  show_point_text_->attach(qwtPlot());

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
  delete reference_tracker_;
  if (show_point_marker_ != nullptr) {
    show_point_marker_->detach();
    delete show_point_marker_;
  }
  if (show_point_text_ != nullptr) {
    show_point_text_->detach();
    delete show_point_text_;
  }
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

  const auto descriptor = catalog_->curveDescriptor(name);
  if (!descriptor.has_value()) {
    return nullptr;
  }

  // Auto-assignment (issue #68): reuse the curve's remembered color so it stays
  // consistent across plots; otherwise take the next color from the shared,
  // session-wide palette counter and remember it. An explicit (non-transparent)
  // color — e.g. from a layout file — is honored as-is. session_ is non-null here
  // (guarded above), so the session registry is always available.
  if (color == Qt::transparent) {
    CurveColorRegistry& registry = session_->curveColorRegistry();
    if (const auto remembered = registry.color(name); remembered.has_value()) {
      color = QColor(*remembered);
    } else {
      color = PlotWidgetBase::paletteColor(registry.nextPaletteIndex());
      registry.setColor(name, color.name());
    }
  }

  auto* adapter = new DatastoreCurveAdapter(session_, *descriptor);
  auto* info = PlotWidgetBase::addCurve(name, adapter, color, curveDisplayName(*descriptor));
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

  const auto x_descriptor = catalog_->curveDescriptor(x_name);
  const auto y_descriptor = catalog_->curveDescriptor(y_name);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  const QString title = tr("%1 vs %2").arg(curveDisplayName(*y_descriptor), curveDisplayName(*x_descriptor));
  auto* series = new PointSeriesXY(session_, *x_descriptor, *y_descriptor);
  auto* info = PlotWidgetBase::addCurve(title, series, color);
  if (info == nullptr) {
    return nullptr;
  }
  if (tracker_ != nullptr) {
    tracker_->setEnabled(false);
  }
  // Clears blue tracker + red tracker's reference_pos_. Otherwise, returning
  // to time-series mode later would resurrect stale Δ values without a blue line.
  setReferenceLine(std::nullopt);
  info->curve->setStyle(QwtPlotCurve::Dots);
  info->curve->setPen(info->curve->pen().color(), dotWidthValue(lineWidth()));
  updateMaximumZoomArea();
  replot();
  return info;
}

void PlotWidget::setZoomRectangle(QRectF rect, bool emit_signal) {
  if (isXYPlot() && keepRatioXY()) {
    // Every programmatic "return to original zoom" funnels through here
    // (zoomOut, the H/V zoom-outs, XML restore). We keep the requested zoom
    // level (PJ3 re-fits to the data extent) and re-impose the canvas aspect
    // ratio so the XY shape stays 1:1.
    applyRectKeepingRatio(rect);
  } else {
    setAxisScale(QwtPlot::yLeft, rect.bottom(), rect.top());
    setAxisScale(QwtPlot::xBottom, rect.left(), rect.right());
    qwtPlot()->updateAxes();
  }

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

void PlotWidget::setReferenceLine(std::optional<double> reference_x_sec) {
  if (reference_tracker_ == nullptr || tracker_ == nullptr) {
    return;
  }
  if (isXYPlot() || !reference_x_sec.has_value()) {
    reference_tracker_->setEnabled(false);
    tracker_->setReferencePosition(std::nullopt);
  } else {
    const QPointF reference_point(*reference_x_sec, 0.0);
    reference_tracker_->setEnabled(true);
    reference_tracker_->setPosition(reference_point);
    tracker_->setReferencePosition(reference_point);
  }
  replot();
}

bool PlotWidget::trackerEnabled() const noexcept {
  return tracker_enabled_;
}

void PlotWidget::setTrackerParameter(CurveTracker::Parameter parameter) {
  if (tracker_ != nullptr) {
    tracker_->setParameter(parameter);
  }
}

void PlotWidget::setShowPoints(bool show) {
  show_points_ = show;
  if (!show) {
    if (show_point_marker_ != nullptr) {
      show_point_marker_->setVisible(false);
    }
    if (show_point_text_ != nullptr) {
      show_point_text_->setVisible(false);
    }
    replot();
  }
}

bool PlotWidget::showPoints() const noexcept {
  return show_points_;
}

void PlotWidget::showPointValues(QPoint paint_point) {
  if (!show_points_ || show_point_marker_ == nullptr || show_point_text_ == nullptr) {
    return;
  }

  auto paint_to_plot = [this](QPoint p) {
    return QPointF(qwtPlot()->invTransform(QwtPlot::xBottom, p.x()), qwtPlot()->invTransform(QwtPlot::yLeft, p.y()));
  };
  auto plot_to_paint = [this](QPointF p) {
    return QPoint(qwtPlot()->transform(QwtPlot::xBottom, p.x()), qwtPlot()->transform(QwtPlot::yLeft, p.y()));
  };

  const QPointF mouse_in_plot = paint_to_plot(paint_point);
  const int precision = QSettings().value(QStringLiteral("Preferences::precision"), 3).toInt();

  QString text;
  int min_distance_sqr = kHoverHitRadiusPx * kHoverHitRadiusPx;
  bool updated = false;
  QPointF marker_point;
  const QwtPlotItemList curves = qwtPlot()->itemList(QwtPlotItem::Rtti_PlotCurve);
  for (auto* item : curves) {
    auto* curve = dynamic_cast<QwtPlotCurve*>(item);
    if (curve == nullptr || !curve->isVisible()) {
      continue;
    }
    const auto maybe_point = curvePointAt(curve, mouse_in_plot.x());
    if (!maybe_point) {
      continue;
    }
    const QPoint sample_paint = plot_to_paint(*maybe_point);
    const QPoint diff = sample_paint - paint_point;
    const int dist_sqr = diff.x() * diff.x() + diff.y() * diff.y();
    if (dist_sqr < min_distance_sqr) {
      updated = true;
      min_distance_sqr = dist_sqr;
      marker_point = *maybe_point;
      text =
          QString("<font color=%1>%2<br>x: %3<br>y: %4</font>")
              .arg(
                  curve->pen().color().name(), curve->title().text(), QString::number(maybe_point->x(), 'f', precision),
                  QString::number(maybe_point->y(), 'f', precision));
    }
  }

  const bool was_visible = show_point_marker_->isVisible();
  show_point_marker_->setVisible(updated);
  show_point_text_->setVisible(updated);

  if (updated) {
    show_point_marker_->setValue(marker_point);

    QwtText label;
    label.setText(text);
    label.setBorderPen(QColor(Qt::transparent));
    QColor background = qwtPlot()->palette().color(QPalette::Window);
    background.setAlpha(220);
    label.setBackgroundBrush(background);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(9);
    label.setFont(font);
    label.setRenderFlags(Qt::AlignLeft);
    show_point_text_->setLabel(label);
    show_point_text_->setLabelAlignment(Qt::AlignRight);

    const QPoint marker_paint = plot_to_paint(marker_point);
    QPoint text_anchor = marker_paint + QPoint(15, -20);
    const double text_width = label.textSize().width();
    const double canvas_width = qwtPlot()->canvas()->width();
    if (marker_paint.x() > canvas_width * 0.5 && (text_anchor.x() + text_width) > canvas_width) {
      text_anchor = marker_paint + QPoint(-15 - static_cast<int>(text_width), -20);
    }
    show_point_text_->setValue(paint_to_plot(text_anchor));
  }

  // Replot only when the steady state actually changed: visibility flipped,
  // we snapped to a different sample, or the tooltip text differs (e.g. a
  // curve appeared/disappeared at the same x).
  const bool visibility_changed = updated != was_visible;
  const bool position_or_text_changed =
      updated && (marker_point != show_point_last_pos_ || text != show_point_last_text_);
  if (visibility_changed || position_or_text_changed) {
    replot();
  }
  show_point_last_pos_ = updated ? marker_point : QPointF{};
  show_point_last_text_ = updated ? text : QString{};
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

  // Skip the <range> element when the canvas has not yet computed a real
  // viewport (e.g. drop happened immediately before save) -- a degenerate
  // rect would restore as a zero-width window and hide everything. Without
  // <range>, xmlLoadState falls back to zoomOut(), which auto-fits.
  const QRectF rect = currentBoundingRect();
  if (rect.left() != rect.right() && rect.top() != rect.bottom()) {
    QDomElement range_element = doc.createElement(QStringLiteral("range"));
    range_element.setAttribute(QStringLiteral("bottom"), QString::number(rect.bottom(), 'f', 6));
    range_element.setAttribute(QStringLiteral("top"), QString::number(rect.top(), 'f', 6));
    range_element.setAttribute(QStringLiteral("left"), QString::number(rect.left(), 'f', 6));
    range_element.setAttribute(QStringLiteral("right"), QString::number(rect.right(), 'f', 6));
    plot_element.appendChild(range_element);
  }

  // A curve is identified by its stable topic+field path (PJ::LayoutXml), not
  // the engine's opaque per-load catalog key. On load — layout file or undo/redo
  // snapshot alike — the path is re-resolved against the current dataset(s) to
  // the concrete key (PJ::LayoutXml::rebindCurveKeys), so the saved form stays
  // valid across reloads and similar datasets.
  const auto writeStablePath = [&](QDomElement& element, const QString& topic_attr, const QString& field_attr,
                                   const QString& key) {
    if (catalog_ == nullptr) {
      return;
    }
    if (const auto descriptor = catalog_->curveDescriptor(key); descriptor.has_value()) {
      element.setAttribute(topic_attr, descriptor->topic_name);
      element.setAttribute(field_attr, descriptor->field_path);
    }
  };

  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QDomElement curve_element = doc.createElement(QStringLiteral("curve"));
    curve_element.setAttribute(QStringLiteral("color"), info.curve->pen().color().name());
    curve_element.setAttribute(QStringLiteral("line_width"), QString::number(info.curve->pen().widthF(), 'f', 2));
    curve_element.setAttribute(QStringLiteral("style"), curveStyleToString(qwtStyleToCurveStyle(info.curve)));
    curve_element.setAttribute(
        QStringLiteral("visible"), info.curve->isVisible() ? QStringLiteral("true") : QStringLiteral("false"));
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      writeStablePath(curve_element, QStringLiteral("x_topic"), QStringLiteral("x_field"), xy_series->xSource().name);
      writeStablePath(curve_element, QStringLiteral("y_topic"), QStringLiteral("y_field"), xy_series->ySource().name);
    } else {
      writeStablePath(curve_element, QStringLiteral("topic"), QStringLiteral("field"), info.source_name);
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
      // Seed the session color memory so this curve keeps its saved color when
      // later dragged into another plot (issue #68). Time-series only — see the
      // matching note in onChangeCurveColor; XY curves are out of scope here.
      if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
        registry->setColor(loaded_curve->source_name, color.name());
      }
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
  QRectF rect;
  if (!range_element.isNull() && autozoom) {
    rect.setBottom(range_element.attribute(QStringLiteral("bottom")).toDouble());
    rect.setTop(range_element.attribute(QStringLiteral("top")).toDouble());
    rect.setLeft(range_element.attribute(QStringLiteral("left")).toDouble());
    rect.setRight(range_element.attribute(QStringLiteral("right")).toDouble());
  }
  // Fall back to zoomOut when no <range> was saved or the saved rect is
  // degenerate (zero-width or zero-height). Without this, an old layout
  // saved before the canvas auto-fitted would restore as a blank plot.
  if (rect.left() == rect.right() || rect.top() == rect.bottom()) {
    zoomOut(false);
  } else {
    setZoomRectangle(rect, false);
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
  CurveInfo* info = curveFromTitle(curve_name);
  if (info != nullptr && info->curve != nullptr) {
    info->curve->setPen(new_color, info->curve->pen().widthF());
    // Remember the override so the curve keeps this color when re-dragged into
    // another plot (issue #68). Keyed by source_name, the same key addCurve uses.
    // Time-series only: XY curves are keyed by a composed title and the registry
    // is never consulted for them (addCurveXY does not auto-assign from it), so
    // writing them here would only add entries nothing reads.
    if (CurveColorRegistry* registry = colorRegistryOf(session_); registry != nullptr && !isXYPlot()) {
      registry->setColor(info->source_name, new_color.name());
    }
    emit curveColorChanged(info->source_name, new_color);
    replot();
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

bool PlotWidget::revalidate() {
  if (catalog_ == nullptr) {
    return false;
  }
  // A curve is stale once its source key is gone from the catalog. Collect first,
  // then remove: removeCurve() mutates curve_list, so removing while iterating
  // would invalidate the iterator.
  QStringList to_remove;
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      if (!catalog_->curveDescriptor(adapter->source().name).has_value()) {
        to_remove.push_back(info.source_name);
      }
    } else if (const auto* xy_series = dynamic_cast<const PointSeriesXY*>(info.curve->data())) {
      if (!catalog_->curveDescriptor(xy_series->xSource().name).has_value() ||
          !catalog_->curveDescriptor(xy_series->ySource().name).has_value()) {
        to_remove.push_back(info.source_name);
      }
    }
  }
  if (to_remove.isEmpty()) {
    return false;
  }
  for (const QString& source_name : to_remove) {
    removeCurve(source_name);
  }
  // If revalidation drained every curve, mirror removeAllCurves()'s reset out of
  // XY mode: onDragEnterEvent() only accepts an add_curve drop when !isXYPlot(),
  // so a stuck-XY empty plot would reject every new curve dropped onto it.
  if (curveList().empty()) {
    setModeXY(false);
    if (tracker_ != nullptr) {
      tracker_->setEnabled(tracker_enabled_);
      tracker_->redraw();
    }
  }
  updateMaximumZoomArea();
  replot();
  return true;
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
    // Mouse hover inspector (buttonShowpoint). Doesn't consume the event so
    // panning/zooming/etc keep working underneath. Gate by show_points_ so
    // the hot path stays a single bool read when the toggle is off.
    if (show_points_) {
      showPointValues(mouse_event->pos());
    }
  }
  if (event->type() == QEvent::Leave && obj == qwtPlot()->canvas()) {
    if (show_point_marker_ != nullptr && show_point_marker_->isVisible()) {
      show_point_marker_->setVisible(false);
      show_point_text_->setVisible(false);
      replot();
    }
  }
  return false;
}

void PlotWidget::onExternallyResized(const QRectF& rect) {
  if (isXYPlot()) {
    if (keepRatioXY()) {
      // Wheel-zoom (magnifier) and pan bypass the drag-zoom keep-ratio path,
      // and the magnifier clamps each axis independently at the data bounds —
      // that asymmetry skews a 1:1 circle into an ellipse. Re-impose the
      // canvas aspect ratio on the event's rect (not currentBoundingRect():
      // the magnifier emits this signal before it replots, so the current
      // view is still stale here).
      applyRectKeepingRatio(rect);
      replot();
    }
    return;  // XY never emits rectChanged (PJ3 parity).
  }
  if (!isZoomLinkEnabled()) {
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
  // Icons are re-applied with the current theme each time the
  // context menu opens (see canvasContextMenuTriggered), so they
  // don't need to be set here.
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
  menu.setObjectName(QStringLiteral("PJMenu"));
  // Refresh icons with the active theme on every popup so the
  // glyphs stay correctly tinted after a theme switch.
  const QString theme = currentTheme();
  action_split_horizontal_->setIcon(QIcon(LoadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(LoadSvg(":/resources/svg/add_row.svg", theme)));
  action_remove_all_curves_->setIcon(QIcon(LoadSvg(":/resources/svg/delete_forever.svg", theme)));
  action_zoom_out_->setIcon(QIcon(LoadSvg(":/resources/svg/zoom_max.svg", theme)));
  action_zoom_out_horizontal_->setIcon(QIcon(LoadSvg(":/resources/svg/zoom_horizontal.svg", theme)));
  action_zoom_out_vertical_->setIcon(QIcon(LoadSvg(":/resources/svg/zoom_vertical.svg", theme)));
  if (selected_curve != nullptr) {
    menu.addAction(
        QIcon(LoadSvg(":/resources/svg/color_background.svg", theme)), tr("Change color..."), this,
        [this, selected_curve]() {
          const QColor current_color = selected_curve->curve->pen().color();
          const QColor next_color = QColorDialog::getColor(current_color, this, tr("Pick curve color"));
          if (next_color.isValid()) {
            onChangeCurveColor(selected_curve->source_name, next_color);
            emit undoableChange();
          }
        });
    menu.addAction(
        QIcon(LoadSvg(":/resources/svg/trash.svg", theme)), tr("Remove curve"), this, [this, selected_curve]() {
          removeCurve(selected_curve->source_name);
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
  if (samples_ingested_connection_) {
    disconnect(samples_ingested_connection_);
  }
  if (dataset_replace_connection_) {
    disconnect(dataset_replace_connection_);
  }
  if (session_ == nullptr) {
    return;
  }

  samples_ingested_connection_ =
      connect(session_, &SessionManager::samplesIngested, this, [this](const QVector<TopicId>& ids, bool live) {
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
        if (!changed) {
          return;
        }
        if (live) {
          // Follow-live: re-fit axes so streaming samples beyond the initial
          // drop-time range become visible. resetZoom subsumes
          // updateMaximumZoomArea + replot.
          resetZoom();
        } else {
          // One-shot writers (file load) stay zoomed where the user left them.
          updateMaximumZoomArea();
          replot();
        }
      });

  // In-place reload swaps a dataset's chunks under our adapters, freeing the raw
  // TopicChunk* they cache (sample_index_ / xy index). Drop those caches
  // synchronously BEFORE the swap to avoid a UAF; bindings (DatasetId/TopicIds)
  // stay valid and re-index on the next paint via the post-swap samplesIngested.
  // Forced DirectConnection so the clear runs inline within the emit (same
  // thread, never queued) — the no-event-loop UAF contract requires it.
  dataset_replace_connection_ = connect(
      session_, &SessionManager::datasetAboutToBeReplaced, this,
      [this](DatasetId dataset_id) {
        for (auto& info : curveList()) {
          if (auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data())) {
            if (adapter->source().dataset_id == dataset_id) {
              adapter->onDataCleared();
            }
            continue;
          }
          if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
            if (xy_series->xSource().dataset_id == dataset_id || xy_series->ySource().dataset_id == dataset_id) {
              xy_series->onDataCleared();
            }
          }
        }
      },
      Qt::DirectConnection);
}

}  // namespace PJ
