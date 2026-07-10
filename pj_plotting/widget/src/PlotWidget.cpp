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
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

#include "WidgetClipboard.h"
#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PointSeriesXY.h"
#include "pj_plotting/XYCurveDialog.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveColorRegistry.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/CurveDisplayName.h"
#include "pj_runtime/SessionManager.h"
#include "pj_widgets/CurveTreeView.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ThemeColors.h"
using namespace Qt::StringLiterals;

namespace PJ {
namespace {

constexpr int kHoverHitRadiusPx = 40;

void addActionCategorySeparator(QMenu& menu) {
  const QList<QAction*> actions = menu.actions();
  if (!actions.isEmpty() && !actions.constLast()->isSeparator()) {
    menu.addSeparator();
  }
}

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString curveKey(const QString& source_name, const QString& x_name = {}, const QString& y_name = {}) {
  if (!x_name.isEmpty() || !y_name.isEmpty()) {
    return u"xy:"_s + x_name + u"\n"_s + y_name;
  }
  return u"ts:"_s + source_name;
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
  // Magenta playback tracker, matching the Source Timeline's playhead needle
  // (theme::kPurple) for cross-widget visual consistency. The reference tracker
  // is blue, matching the timeline's blue reference line.
  tracker_ = new CurveTracker(qwtPlot(), PJ::theme::kPurple);
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
  connect(this, &PlotWidgetBase::curveListChanged, this, [this]() {
    updateMaximumZoomArea();
    autoZoomPlotVertically();
  });
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
      // New curve: choose the colour index from the configured sequence.
      // "global" advances the session-wide registry counter (one continuous
      // sequence across every plot); "per plot" uses this curve's positional
      // index within the plot, so each plot restarts at colour 0. Either way the
      // colour is then remembered by name (always-on), so the curve keeps it on
      // re-add and across plots.
      QSettings settings;
      const bool global = settings.value(u"Preferences::curve_color_global"_s, true).toBool();
      const int index = global ? registry.nextPaletteIndex() : static_cast<int>(curveList().size());
      color = PlotWidgetBase::paletteColor(index);
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

void PlotWidget::autoZoomPlotVertically() {
  // Skip during layout restore (the saved range wins), when there is nothing to
  // fit, and for XY plots (their X axis is data, not the shared time axis).
  if (loading_state_ || curveList().empty() || isXYPlot()) {
    return;
  }
  if (!QSettings().value(u"Preferences::auto_zoom_plots"_s, true).toBool()) {
    return;
  }
  // Rescale only Y, over the current X window, so the shared time axis — and
  // therefore the other plots — stay put.
  const Range<double> range_x = getVisualizationRangeX();
  const Range<double> range_y = getVisualizationRangeY(range_x);
  // Guard against a non-finite or degenerate range (e.g. a curve that is all
  // NaN/inf over the window) — feeding it to setAxisScale yields a blank axis.
  if (!std::isfinite(range_y.min) || !std::isfinite(range_y.max) || range_y.min >= range_y.max) {
    return;
  }
  setAxisScale(QwtPlot::yLeft, range_y.min, range_y.max);
  replot();
}

void PlotWidget::replaceCurve(const QString& source_key, const QString& output_key) {
  // No-op (header contract) if services are unset or the output is not in the catalog: never
  // drop the existing source curve when the replacement could not actually be added.
  if (session_ == nullptr || catalog_ == nullptr || !catalog_->curveDescriptor(output_key).has_value()) {
    return;
  }
  // Inherit the source curve's colour, if it is currently plotted, so the
  // filtered output takes its place with the same colour (PJ3 transform-in-place).
  QColor color = Qt::transparent;
  for (const CurveInfo& info : curveList()) {
    if (info.source_name == source_key && info.curve != nullptr) {
      color = info.curve->pen().color();
      break;
    }
  }
  removeCurve(source_key);      // no-op if the source is not currently plotted
  addCurve(output_key, color);  // explicit colour honoured as-is; transparent => palette
  replot();
}

PlotWidget::CurveInfo* PlotWidget::addCurveXY(
    const QString& x_name, const QString& y_name, const QString& alias, QColor color) {
  if (session_ == nullptr || catalog_ == nullptr) {
    return nullptr;
  }

  const auto x_descriptor = catalog_->curveDescriptor(x_name);
  const auto y_descriptor = catalog_->curveDescriptor(y_name);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  // The alias is the curve's display title (legend + the title curveFromTitle()
  // keys on). Empty alias → auto "Y vs X" title (legacy / non-interactive callers).
  const QString title =
      alias.isEmpty() ? tr("%1 vs %2").arg(curveDisplayName(*y_descriptor), curveDisplayName(*x_descriptor)) : alias;
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
  // Style/width are plot-level: addCurve() above already applied the plot's
  // current style and width. XY plots are not forced to Dots — they inherit the
  // plot-level style like any plot (PJ3 parity); the Curve Style toolbar controls it.
  updateMaximumZoomArea();
  // Reveal the ride-along marker at the current cursor immediately rather than on
  // the next seek/tick (setTrackerPosition replots).
  setTrackerPosition(last_tracker_time_sec_);
  return info;
}

PlotWidget::CurveInfo* PlotWidget::createCurveXYInteractive(const QString& x_key, const QString& y_key) {
  if (catalog_ == nullptr) {
    return nullptr;
  }
  const auto x_descriptor = catalog_->curveDescriptor(x_key);
  const auto y_descriptor = catalog_->curveDescriptor(y_key);
  if (!x_descriptor.has_value() || !y_descriptor.has_value()) {
    return nullptr;
  }

  XYCurveDialog dialog(curveDisplayName(*x_descriptor), curveDisplayName(*y_descriptor), this);
  while (dialog.exec() == QDialog::Accepted) {
    const QString alias = dialog.alias();
    if (curveFromTitle(alias) != nullptr) {
      MessageBox::warning(
          this, tr("Duplicate name"), tr("A curve named \"%1\" already exists in this plot.").arg(alias));
      continue;
    }
    // The dialog reports whether the user swapped X and Y relative to the arguments.
    const QString final_x = dialog.swapped() ? y_key : x_key;
    const QString final_y = dialog.swapped() ? x_key : y_key;
    return addCurveXY(final_x, final_y, alias);
  }
  return nullptr;  // cancelled
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

CurveTracker::Parameter PlotWidget::trackerParameter() const noexcept {
  return tracker_ != nullptr ? tracker_->parameter() : CurveTracker::kValue;
}

bool PlotWidget::trackerValueBoxVisible() const noexcept {
  return tracker_ != nullptr && tracker_->valueBoxVisible();
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
  const int precision = QSettings().value(u"Preferences::precision"_s, 3).toInt();

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
  QDomElement plot_element = doc.createElement(u"plot"_s);
  plot_element.setAttribute(u"id"_s, state_id_);
  plot_element.setAttribute(u"mode"_s, isXYPlot() ? u"XYPlot"_s : u"TimeSeries"_s);
  plot_element.setAttribute(u"line_width"_s, lineWidthToString(lineWidth()));
  // Style and width are plot-level properties (every curve shares them; only
  // colour is per-curve), so they are saved once on the <plot>, not per <curve>.
  plot_element.setAttribute(u"style"_s, curveStyleToString(curveStyle()));
  plot_element.setAttribute(u"title"_s, qwtPlot()->title().text());
  plot_element.setAttribute(u"tracker_enabled"_s, tracker_enabled_ ? u"true"_s : u"false"_s);

  // Skip the <range> element when the canvas has not yet computed a real
  // viewport (e.g. drop happened immediately before save) -- a degenerate
  // rect would restore as a zero-width window and hide everything. Without
  // <range>, xmlLoadState falls back to zoomOut(), which auto-fits.
  const QRectF rect = currentBoundingRect();
  if (rect.left() != rect.right() && rect.top() != rect.bottom()) {
    QDomElement range_element = doc.createElement(u"range"_s);
    range_element.setAttribute(u"bottom"_s, QString::number(rect.bottom(), 'f', 6));
    range_element.setAttribute(u"top"_s, QString::number(rect.top(), 'f', 6));
    // The X axis of a time-series plot is TIME: persist it in ABSOLUTE seconds,
    // not the display-relative seconds the Qwt axis speaks (display = absolute -
    // offset; see pj_runtime/Time.h). PJ4's display offset is per-dataset and
    // live, so a display-relative range only frames the right instant for the
    // offset present at save time — storing absolute makes the restored range
    // correct regardless of the offset state at load (the "Use time offset"
    // toggle, reloaded data, a layout shared between machines). The time axis is
    // ALWAYS stored absolute — no marker is written; on load the plot MODE
    // (time-series vs XY) is what decides whether to undo the offset. XY plots' X is
    // a value, not time, so they keep their raw axis coordinates.
    if (isXYPlot()) {
      range_element.setAttribute(u"left"_s, QString::number(rect.left(), 'f', 6));
      range_element.setAttribute(u"right"_s, QString::number(rect.right(), 'f', 6));
    } else {
      const double offset_sec = displayOffsetSeconds();
      range_element.setAttribute(u"left"_s, QString::number(rect.left() + offset_sec, 'f', 6));
      range_element.setAttribute(u"right"_s, QString::number(rect.right() + offset_sec, 'f', 6));
    }
    plot_element.appendChild(range_element);
  }

  // A curve is identified by its stable topic+field path (PJ::LayoutXml), not
  // the engine's opaque per-load catalog key. On load — layout file or undo/redo
  // snapshot alike — the path is re-resolved against the current dataset(s) to
  // the concrete key (PJ::LayoutXml::rebindCurveKeys), so the saved form stays
  // valid across reloads and similar datasets.
  const auto write_stable_path = [&](QDomElement& element, const QString& topic_attr, const QString& field_attr,
                                     const QString& dataset_id_attr, const QString& dataset_source_attr,
                                     const QString& key) {
    if (catalog_ == nullptr) {
      return;
    }
    if (const auto descriptor = catalog_->curveDescriptor(key); descriptor.has_value()) {
      element.setAttribute(topic_attr, descriptor->topic_name);
      element.setAttribute(field_attr, descriptor->field_path);
      // Exact-id + raw-source qualifiers so a same-topic sibling dataset is never
      // interchanged on restore. The raw DatasetInfo::source_name is preferred
      // over the deduped display label (dataset_name) because it is the portable
      // identity a later-session reload compares against.
      element.setAttribute(dataset_id_attr, QString::number(descriptor->dataset_id));
      element.setAttribute(
          dataset_source_attr, catalog_->datasetSourceName(descriptor->dataset_id).value_or(descriptor->dataset_name));
    }
  };

  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    QDomElement curve_element = doc.createElement(u"curve"_s);
    curve_element.setAttribute(u"color"_s, info.curve->pen().color().name());
    curve_element.setAttribute(u"visible"_s, info.curve->isVisible() ? u"true"_s : u"false"_s);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      // An XY curve's title is the user alias (not derivable from x/y), so persist it.
      curve_element.setAttribute(u"name"_s, info.source_name);
      write_stable_path(
          curve_element, u"x_topic"_s, u"x_field"_s, u"x_dataset_id"_s, u"x_dataset_source"_s,
          xy_series->xSource().name);
      write_stable_path(
          curve_element, u"y_topic"_s, u"y_field"_s, u"y_dataset_id"_s, u"y_dataset_source"_s,
          xy_series->ySource().name);
    } else {
      write_stable_path(curve_element, u"topic"_s, u"field"_s, u"dataset_id"_s, u"dataset_source"_s, info.source_name);
    }
    plot_element.appendChild(curve_element);
  }

  return plot_element;
}

bool PlotWidget::xmlLoadState(const QDomElement& plot_element, bool autozoom) {
  if (plot_element.isNull() || plot_element.tagName() != u"plot"_s) {
    return false;
  }

  setStateId(plot_element.attribute(u"id"_s));
  setModeXY(plot_element.attribute(u"mode"_s) == u"XYPlot"_s);
  // Line width is plot-level. New layouts store the chosen width on the <plot>.
  // Older layouts kept the plot-level value at the stale default ("1.0") and the
  // real width per-curve, so when the plot value is absent/default fall back to
  // the first saved curve's pixel width. New layouts no longer write a per-curve
  // line_width, so the fallback only fires for genuinely old files.
  const QString plot_line_width = plot_element.attribute(u"line_width"_s);
  const QDomElement first_curve = plot_element.firstChildElement(u"curve"_s);
  if ((plot_line_width.isEmpty() || plot_line_width == u"1.0"_s) && first_curve.hasAttribute(u"line_width"_s)) {
    setLineWidth(lineWidthFromPixels(first_curve.attribute(u"line_width"_s).toDouble()));
  } else {
    setLineWidth(lineWidthFromString(plot_line_width.isEmpty() ? u"1.0"_s : plot_line_width));
  }
  // Style is plot-level. New layouts store it on the <plot>; for older layouts
  // that stored it per <curve>, fall back to the first saved curve. setDefaultStyle
  // restyles the plot and is inherited by curves added after the load.
  QString style_attr = plot_element.attribute(u"style"_s);
  if (style_attr.isEmpty()) {
    style_attr = first_curve.attribute(u"style"_s, u"Lines"_s);
  }
  setDefaultStyle(curveStyleFromString(style_attr));
  setTrackerEnabled(plot_element.attribute(u"tracker_enabled"_s, u"true"_s) == u"true"_s);
  qwtPlot()->setTitle(plot_element.attribute(u"title"_s));

  const bool was_loading_state = loading_state_;
  loading_state_ = true;

  std::set<QString> desired_keys;
  for (QDomElement curve_element = plot_element.firstChildElement(u"curve"_s); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(u"curve"_s)) {
    if (isXYPlot() && curve_element.hasAttribute(u"curve_x"_s) && curve_element.hasAttribute(u"curve_y"_s)) {
      desired_keys.insert(curveKey(
          curve_element.attribute(u"name"_s), curve_element.attribute(u"curve_x"_s),
          curve_element.attribute(u"curve_y"_s)));
    } else {
      desired_keys.insert(curveKey(curve_element.attribute(u"name"_s)));
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

  for (QDomElement curve_element = plot_element.firstChildElement(u"curve"_s); !curve_element.isNull();
       curve_element = curve_element.nextSiblingElement(u"curve"_s)) {
    applyCurveElement(curve_element);
  }

  // Stash the layout-saved viewport (raw, pre-offset-conversion) and frame to it via
  // the shared helper. During a progressive restore the catalog is still empty here,
  // so the absolute->display conversion is wrong until the dataset binds — the
  // progressive path re-applies the stash per curve-bind and at drain (see
  // applySavedViewportOrZoom / PendingCurveBinder). A blocking load resolves correctly
  // on this first call because the offset is already known.
  const QDomElement range_element = plot_element.firstChildElement(u"range"_s);
  if (!range_element.isNull() && autozoom) {
    saved_viewport_ = SavedViewport{
        range_element.attribute(u"bottom"_s).toDouble(), range_element.attribute(u"top"_s).toDouble(),
        range_element.attribute(u"left"_s).toDouble(), range_element.attribute(u"right"_s).toDouble()};
  } else {
    saved_viewport_.reset();
  }
  applySavedViewportOrZoom(/*clear_after=*/false);
  replot();
  loading_state_ = was_loading_state;
  return true;
}

void PlotWidget::applySavedViewportOrZoom(bool clear_after) {
  QRectF rect;
  // The layout always stores a time axis in ABSOLUTE seconds; the per-dataset display
  // offset is a visualization concern applied HERE, never persisted. Convert the saved X
  // back to the CURRENT display coordinate (display = absolute - offset). An XY plot's X
  // is a data value, not time, so it is offset-independent and used verbatim. Re-running
  // this as the offset settles is what lets a progressive restore pin the saved window
  // up front and keep it framed while data streams in.
  if (saved_viewport_.has_value()) {
    const SavedViewport& view = *saved_viewport_;
    double left = view.left;
    double right = view.right;
    if (!isXYPlot()) {
      const double offset_sec = displayOffsetSeconds();
      left -= offset_sec;
      right -= offset_sec;
    }
    rect.setBottom(view.bottom);
    rect.setTop(view.top);
    rect.setLeft(left);
    rect.setRight(right);
  }
  // Degenerate or no saved range -> auto-fit (fresh load, or a layout saved before the
  // canvas computed a viewport).
  if (rect.left() == rect.right() || rect.top() == rect.bottom()) {
    zoomOut(/*emit_signal=*/false);
  } else {
    setZoomRectangle(rect, /*emit_signal=*/false);
  }
  if (clear_after) {
    saved_viewport_.reset();
  }
}

PlotWidget::CurveInfo* PlotWidget::applyCurveElement(const QDomElement& curve_element) {
  const QColor color(curve_element.attribute(u"color"_s));
  CurveInfo* loaded_curve = nullptr;
  if (isXYPlot() && curve_element.hasAttribute(u"curve_x"_s) && curve_element.hasAttribute(u"curve_y"_s)) {
    const QString x_name = curve_element.attribute(u"curve_x"_s);
    const QString y_name = curve_element.attribute(u"curve_y"_s);
    const QString source_name = curve_element.attribute(u"name"_s);
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
      // Restore the saved alias as the title; no dialog on load.
      loaded_curve = addCurveXY(x_name, y_name, source_name, color.isValid() ? color : Qt::transparent);
    }
  } else {
    const QString curve_name = curve_element.attribute(u"name"_s);
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
  // Style and width are plot-level (restored once in xmlLoadState); only colour
  // and visibility are per-curve.
  if (loaded_curve != nullptr) {
    const QString visible_attr = curve_element.attribute(u"visible"_s, u"true"_s);
    loaded_curve->curve->setVisible(visible_attr == u"true"_s);
  }
  return loaded_curve;
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
  last_tracker_time_sec_ = display_time_sec;
  tracker_->setEnabled(tracker_enabled_ && !isXYPlot());
  if (isXYPlot()) {
    // An XY plot has no time axis, so the vertical-line tracker is meaningless.
    // Instead each curve's marker rides along the parametric curve, sitting on
    // the (x,y) sample at the current time (PJ3 parity).
    for (CurveInfo& info : curveList()) {
      if (info.marker == nullptr || info.curve == nullptr) {
        continue;
      }
      const auto* series = dynamic_cast<const PointSeriesXY*>(info.curve->data());
      std::optional<QPointF> point;
      if (series != nullptr) {
        point = series->sampleFromTime(display_time_sec);
      }
      if (point.has_value() && info.curve->isVisible()) {
        info.marker->setValue(*point);
        info.marker->setVisible(true);
      } else {
        info.marker->setVisible(false);
      }
    }
    replot();
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
  if (!loading_state_) {
    emit undoableChange();
  }
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
  if (curveList().empty()) {
    return;
  }
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
  if (mime_data->hasFormat(u"curveslist/add_curve"_s)) {
    const QStringList curves = decodeCurveDrop(mime_data, u"curveslist/add_curve"_s);
    if (!curves.empty() && allCurvesDroppable(curves) && !isXYPlot()) {
      dragging_.mode = DragMode::kCurves;
      dragging_.curves = curves;
      event->acceptProposedAction();
    }
    return;
  }

  if (mime_data->hasFormat(CurveTreeView::newXyAxisMimeType())) {
    const QStringList curves = decodeCurveDrop(mime_data, CurveTreeView::newXyAxisMimeType());
    // Accept a new XY pair on an empty plot OR on a plot that is already XY (a plot
    // can hold several XY curves) — but never mixed with time-series curves.
    if (curves.size() == 2 && allCurvesKnown(curves) && (curveList().empty() || isXYPlot())) {
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
        if (isPlaceholderCurveName(curve_name)) {
          // No fabricated curve — the shell (TopicDemandController) registers
          // demand and a pending bind that completes once real data arrives.
          emit placeholderCurveDropped(curve_name);
          curves_changed = true;
          continue;
        }
        curves_changed = addCurve(curve_name) != nullptr || curves_changed;
      }
    }
  } else if (dragging_.mode == DragMode::kNewXY && dragging_.curves.size() == 2) {
    if (!isXYPlot() && !curveList().empty()) {
      emit statusMessageRequested(tr("Create XY plots by dropping two curves on an empty plot."));
    } else {
      const bool was_xy = isXYPlot();
      setModeXY(true);
      curves_changed = createCurveXYInteractive(dragging_.curves[0], dragging_.curves[1]) != nullptr;
      if (!curves_changed && !was_xy) {
        // Cancelled the dialog on a plot that was not already XY: don't strand the
        // (still empty) plot in XY mode, or it would silently refuse normal drops.
        setModeXY(false);
      }
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

void PlotWidget::copyWidgetToClipboard() {
  QDomDocument doc(u"plotjuggler_widget"_s);
  QDomElement plot_element = xmlSaveState(doc);
  if (plot_element.isNull()) {
    return;
  }
  stampClipboardCurveKeys(plot_element);
  doc.appendChild(plot_element);
  widget_clipboard::setXml(doc.toString(2));
}

void PlotWidget::pasteWidgetFromClipboard() {
  QDomDocument doc;
  if (!widget_clipboard::parse(doc, u"plot"_s)) {
    return;
  }
  QDomElement plot_element = doc.documentElement();
  plot_element.setAttribute(u"id"_s, state_id_);
  rebindClipboardCurveKeys(plot_element);
  if (xmlLoadState(plot_element)) {
    emit undoableChange();
  }
}

bool PlotWidget::canPasteWidgetFromClipboard() const {
  QDomDocument doc;
  return widget_clipboard::parse(doc, u"plot"_s);
}

void PlotWidget::stampClipboardCurveKeys(QDomElement& plot_element) const {
  // Full-path qualifier so a clipboard paste in a later session can validate a
  // reminted id (see rebindClipboardCurveKeys). xmlSaveState already stamped
  // dataset_id + dataset_source; the path is the tiebreak for same-basename files.
  const auto stamp_path = [this](QDomElement& curve, const QString& path_attr, DatasetId dataset_id) {
    if (catalog_ == nullptr) {
      return;
    }
    const QString path = catalog_->datasetSourcePath(dataset_id);
    if (!path.isEmpty()) {
      curve.setAttribute(path_attr, path);
    }
  };

  QDomElement curve_element = plot_element.firstChildElement(u"curve"_s);
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr || curve_element.isNull()) {
      continue;
    }
    curve_element.setAttribute(u"name"_s, info.source_name);
    if (auto* xy_series = dynamic_cast<PointSeriesXY*>(info.curve->data())) {
      curve_element.setAttribute(u"curve_x"_s, xy_series->xSource().name);
      curve_element.setAttribute(u"curve_y"_s, xy_series->ySource().name);
      stamp_path(curve_element, u"x_dataset_path"_s, xy_series->xSource().dataset_id);
      stamp_path(curve_element, u"y_dataset_path"_s, xy_series->ySource().dataset_id);
    } else if (catalog_ != nullptr) {
      if (const auto descriptor = catalog_->curveDescriptor(info.source_name); descriptor.has_value()) {
        stamp_path(curve_element, u"dataset_path"_s, descriptor->dataset_id);
      }
    }
    curve_element = curve_element.nextSiblingElement(u"curve"_s);
  }
}

void PlotWidget::rebindClipboardCurveKeys(QDomElement& plot_element) const {
  if (catalog_ == nullptr) {
    return;
  }
  // Clipboard XML carries the same dataset qualifiers as any snapshot (dataset_id +
  // dataset_source + full dataset_path, written by xmlSaveState/stampClipboardCurveKeys),
  // and the clipboard outlives undo-history epochs: a pasted id may be stale or reminted.
  // Three-way result:
  //   value  = resolved concrete key -> set it;
  //   empty  = the curve WAS qualified but resolution failed -> CLEAR the concrete key,
  //            so a stale opaque key from another session can never collide with a live
  //            different-dataset series;
  //   nullopt = unqualified legacy copy that stays ambiguous -> leave the copied key
  //             intact (today's behavior; xmlLoadState drops it if it no longer resolves).
  const auto resolve = [this](
                           const QDomElement& curve, const QString& topic_attr, const QString& field_attr,
                           const QString& id_attr, const QString& source_attr,
                           const QString& path_attr) -> std::optional<QString> {
    const QString topic = curve.attribute(topic_attr);
    const QString field = curve.attribute(field_attr);
    if (topic.isEmpty() || field.isEmpty()) {
      return std::nullopt;
    }
    const DatasetId dataset_id = curve.attribute(id_attr).toUInt();
    const QString dataset_source = curve.attribute(source_attr);
    const QString dataset_path = curve.attribute(path_attr);
    // Shared resolver (same three tiers as layout/undo restore, incl. the
    // full-path-fallback leg the local scan used to lack) — a resolved key comes
    // back concrete. A miss is nullopt, so map it to the 3-way apply below by
    // re-deriving whether the curve WAS qualified: qualified-miss clears the key
    // (never let a stale opaque key collide with a live different-dataset series),
    // unqualified-miss leaves it untouched (today's behavior).
    const auto key = catalog_->resolveCurveKey(dataset_id, dataset_source, dataset_path, topic, field);
    if (key.has_value()) {
      return key;
    }
    const bool was_qualified = dataset_id != 0 || !dataset_source.isEmpty() || !dataset_path.isEmpty();
    return was_qualified ? std::optional<QString>{QString{}} : std::nullopt;
  };

  // Applies a resolve() result to one key attribute: overwrite on success, remove
  // on qualified failure (empty), leave untouched when the result is nullopt.
  const auto apply_key = [](QDomElement& curve, const QString& key_attr, const std::optional<QString>& result) {
    if (!result.has_value()) {
      return;
    }
    if (result->isEmpty()) {
      curve.removeAttribute(key_attr);
    } else {
      curve.setAttribute(key_attr, *result);
    }
  };

  for (QDomElement curve = plot_element.firstChildElement(u"curve"_s); !curve.isNull();
       curve = curve.nextSiblingElement(u"curve"_s)) {
    if (curve.hasAttribute(u"x_topic"_s)) {
      const std::optional<QString> x_key =
          resolve(curve, u"x_topic"_s, u"x_field"_s, u"x_dataset_id"_s, u"x_dataset_source"_s, u"x_dataset_path"_s);
      const std::optional<QString> y_key =
          resolve(curve, u"y_topic"_s, u"y_field"_s, u"y_dataset_id"_s, u"y_dataset_source"_s, u"y_dataset_path"_s);
      // An XY curve is undrawable unless BOTH axes resolve. If either axis was
      // qualified-but-failed, clear both keys so no stale half survives.
      if (x_key.has_value() && !x_key->isEmpty() && y_key.has_value() && !y_key->isEmpty()) {
        curve.setAttribute(u"curve_x"_s, *x_key);
        curve.setAttribute(u"curve_y"_s, *y_key);
      } else if ((x_key.has_value() && x_key->isEmpty()) || (y_key.has_value() && y_key->isEmpty())) {
        curve.removeAttribute(u"curve_x"_s);
        curve.removeAttribute(u"curve_y"_s);
      }
      continue;
    }
    apply_key(
        curve, u"name"_s,
        resolve(curve, u"topic"_s, u"field"_s, u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s));
  }
}

void PlotWidget::canvasContextMenuTriggered(const QPoint& pos) {
  if (!context_menu_enabled_) {
    return;
  }

  QMenu menu(qwtPlot());
  menu.setObjectName(u"PJMenu"_s);
  menu.setProperty("categorySeparators", true);
  // Refresh icons with the active theme on every popup so the
  // glyphs stay correctly tinted after a theme switch.
  const QString theme = currentTheme();
  action_split_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/add_column.svg", theme)));
  action_split_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/add_row.svg", theme)));
  action_remove_all_curves_->setIcon(QIcon(loadSvg(":/resources/svg/delete_forever.svg", theme)));
  action_zoom_out_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_max.svg", theme)));
  action_zoom_out_horizontal_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_horizontal.svg", theme)));
  action_zoom_out_vertical_->setIcon(QIcon(loadSvg(":/resources/svg/zoom_vertical.svg", theme)));
  // Apply Filter...: open the Filter Editor scoped to this plot's curves. On
  // Save it applies the chosen filter (via DataProcessorService) and adds the
  // resulting filtered curve(s) to this plot. Filters are time-series transforms,
  // so the action is hidden on XY (scatter) plots.
  if (session_ != nullptr && catalog_ != nullptr && !curveList().empty() && !isXYPlot()) {
    menu.addAction(QIcon(loadSvg(":/resources/svg/function.svg", theme)), tr("Apply Filter..."), this, [this]() {
      launchFilterEditor();
    });
    addActionCategorySeparator(menu);
  }

  menu.addAction(
      QIcon(loadSvg(":/resources/svg/copy.svg", theme)), tr("Copy"), this, [this]() { copyWidgetToClipboard(); });
  QAction* paste_action = menu.addAction(
      QIcon(loadSvg(":/resources/svg/paste.svg", theme)), tr("Paste"), this, [this]() { pasteWidgetFromClipboard(); });
  paste_action->setEnabled(canPasteWidgetFromClipboard());
  addActionCategorySeparator(menu);

  menu.addAction(action_split_horizontal_);
  menu.addAction(action_split_vertical_);
  addActionCategorySeparator(menu);
  menu.addAction(action_zoom_out_);
  menu.addAction(action_zoom_out_horizontal_);
  menu.addAction(action_zoom_out_vertical_);
  addActionCategorySeparator(menu);
  menu.addAction(action_remove_all_curves_);
  action_remove_all_curves_->setEnabled(!curveList().empty());
  menu.exec(qwtPlot()->canvas()->mapToGlobal(pos));
}

void PlotWidget::launchFilterEditor() {
  if (session_ == nullptr || catalog_ == nullptr) {
    return;
  }
  std::vector<CurveDescriptor> sources;
  for (const CurveInfo& info : curveList()) {
    if (const auto descriptor = catalog_->curveDescriptor(info.source_name)) {
      sources.push_back(*descriptor);
    }
  }
  if (sources.empty()) {
    return;
  }
  // The Filter Editor is a chart-area takeover panel owned by the host (it needs
  // MainWindow::presentPanel), so request it rather than constructing it here.
  // On Apply the host calls replaceCurve() on this plot for each result.
  emit filterEditorRequested(std::move(sources), this);
}

void PlotWidget::setAxisScale(QwtAxisId axis_id, double min, double max) {
  if (min > max) {
    std::swap(min, max);
  }
  qwtPlot()->setAxisScale(axis_id, min, max);
}

double PlotWidget::displayOffsetSeconds() const {
  if (session_ == nullptr) {
    return 0.0;
  }
  // The axis is shared across curves; in the common case they share a dataset
  // (hence one offset). When they don't, the first datastore-backed curve's
  // dataset is the representative — the same rule must hold at save and load so
  // the absolute<->display round-trip is stable.
  for (const CurveInfo& info : curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      const DisplayOffset offset = session_->displayOffset(adapter->source().dataset_id);
      return std::chrono::duration<double>(offset.value).count();
    }
  }
  return 0.0;
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

bool PlotWidget::isPlaceholderCurveName(const QString& name) const {
  if (catalog_ == nullptr) {
    return false;
  }
  const auto item = catalog_->itemDescriptor(name);
  return item.has_value() && isAdvertisedTopic(*item) &&
         asAdvertisedTopic(*item)->classification == sdk::BuiltinObjectType::kNone;
}

bool PlotWidget::allCurvesDroppable(const QStringList& curves) const {
  if (catalog_ == nullptr) {
    return false;
  }
  return std::all_of(curves.begin(), curves.end(), [this](const QString& curve_name) {
    return catalog_->curveDescriptor(curve_name).has_value() || isPlaceholderCurveName(curve_name);
  });
}

QString PlotWidget::lineWidthToString(LineWidth width) {
  switch (width) {
    case LineWidth::kPoints10:
      return u"1.0"_s;
    case LineWidth::kPoints15:
      return u"1.5"_s;
    case LineWidth::kPoints20:
      return u"2.0"_s;
    case LineWidth::kPoints30:
      return u"3.0"_s;
  }
  return u"1.0"_s;
}

LineWidth PlotWidget::lineWidthFromString(QString value) {
  if (value == u"1.5"_s) {
    return LineWidth::kPoints15;
  }
  if (value == u"2.0"_s) {
    return LineWidth::kPoints20;
  }
  if (value == u"3.0"_s) {
    return LineWidth::kPoints30;
  }
  return LineWidth::kPoints10;
}

LineWidth PlotWidget::lineWidthFromPixels(double pixels) {
  // Older layouts stored the line width per-curve as a raw pen width, which could be
  // either the rendered lineWidthValue() or the raw toolbar value (lineWidthValue is
  // `scale` times the raw value). Pick the LineWidth closest across both scales,
  // deriving every candidate from lineWidthValue() so the width ladder lives in one place.
  const double scale = lineWidthValue(LineWidth::kPoints10);  // raw == lineWidthValue / scale
  int best = 0;
  double best_dist = std::numeric_limits<double>::max();
  for (int i = static_cast<int>(LineWidth::kPoints10); i <= static_cast<int>(LineWidth::kPoints30); ++i) {
    const double scaled = lineWidthValue(static_cast<LineWidth>(i));
    const double dist = std::min(std::abs(pixels - scaled), std::abs(pixels - scaled / scale));
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return static_cast<LineWidth>(best);
}

QString PlotWidget::curveStyleToString(CurveStyle style) {
  switch (style) {
    case kLines:
      return u"Lines"_s;
    case kDots:
      return u"Dots"_s;
    case kLinesAndDots:
      return u"LinesAndDots"_s;
    case kSticks:
      return u"Sticks"_s;
    case kSteps:
      return u"Steps"_s;
    case kStepsInverted:
      return u"StepsInverted"_s;
  }
  return u"Lines"_s;
}

PlotWidgetBase::CurveStyle PlotWidget::curveStyleFromString(QString value) {
  if (value == u"Dots"_s) {
    return kDots;
  }
  if (value == u"LinesAndDots"_s) {
    return kLinesAndDots;
  }
  if (value == u"Sticks"_s) {
    return kSticks;
  }
  if (value == u"Steps"_s) {
    return kSteps;
  }
  if (value == u"StepsInverted"_s) {
    return kStepsInverted;
  }
  return kLines;
}

void PlotWidget::reconnectDataSignals() {
  if (samples_ingested_connection_) {
    disconnect(samples_ingested_connection_);
  }
  if (dataset_replace_connection_) {
    disconnect(dataset_replace_connection_);
  }
  if (display_offset_connection_) {
    disconnect(display_offset_connection_);
  }
  if (display_offset_dataset_connection_) {
    disconnect(display_offset_dataset_connection_);
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

  // Global "Use time offset" toggled (or otherwise re-based): every curve's x
  // shifts by a constant and NO topic changed, so a per-topic samplesIngested
  // would skip them all. Drop each time-series adapter's cached offset and
  // re-fit, since the prior zoom rect (in display seconds) no longer frames the
  // shifted data.
  display_offset_connection_ = connect(session_, qOverload<>(&SessionManager::displayOffsetChanged), this, [this]() {
    if (invalidateAdapterOffsets()) {
      resetZoom();
    }
    // The curves just moved to the new frame, so each tracker's cached
    // intersection markers (the circles) were sampled against the OLD data and
    // view rect — re-sample them at their current positions against the re-fit
    // curves. Order-independent with the app's reference re-projection: whichever
    // of {reposition, this redraw} runs last re-samples against re-fit data, so
    // the blue line keeps its dots across the toggle.
    if (tracker_ != nullptr) {
      tracker_->redraw();
    }
    if (reference_tracker_ != nullptr) {
      reference_tracker_->redraw();
    }
    replot();
  });

  // ONE source's display offset changed (e.g. a Timeline drag). The samples did
  // not move — only their X mapping. Drop the bound adapters' offset caches and
  // replot at the CURRENT zoom so the curve slides into its new position without
  // re-fitting axes. Unlike datasetAboutToBeReplaced there is no UAF window
  // (chunks stay alive), so a normal (auto/queued) connection is fine.
  // PointSeriesXY ignores display_offset (plan §12) — skip it.
  display_offset_dataset_connection_ = connect(
      session_, qOverload<PJ::DatasetId>(&SessionManager::displayOffsetChanged), this, [this](DatasetId dataset_id) {
        if (invalidateAdapterOffsets(dataset_id)) {
          replot();  // current zoom; the source's curve slides into its new position
        }
      });
}

bool PlotWidget::invalidateAdapterOffsets(std::optional<DatasetId> only) {
  bool changed = false;
  for (auto& info : curveList()) {
    auto* adapter = dynamic_cast<DatastoreCurveAdapter*>(info.curve->data());
    if (adapter == nullptr) {
      continue;  // PointSeriesXY ignores display offset (plan §12)
    }
    if (only.has_value() && adapter->source().dataset_id != *only) {
      continue;
    }
    adapter->onDisplayOffsetChanged();
    changed = true;
  }
  return changed;
}

}  // namespace PJ
