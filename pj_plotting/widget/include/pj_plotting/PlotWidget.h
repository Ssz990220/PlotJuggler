#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_axis_id.h>

#include <QAction>
#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QRectF>
#include <QStringList>
#include <optional>
#include <vector>

#include "pj_plotting/CurveTracker.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_runtime/CurveDescriptor.h"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

namespace PJ {

class CatalogModel;
class SessionManager;

class PlotWidget : public PlotWidgetBase {
  Q_OBJECT
 public:
  explicit PlotWidget(SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  using PlotWidgetBase::addCurve;
  CurveInfo* addCurve(const QString& name, QColor color = Qt::transparent);
  CurveInfo* addCurveXY(const QString& x_name, const QString& y_name, QColor color = Qt::transparent);

  // Replace the curve plotting `source_key` with one plotting `output_key`, the
  // new curve inheriting the source's color and taking its place (PJ3 in-place
  // transform semantics — the Filter Editor's "filtered series replaces the
  // input, same colour"). If `source_key` is absent, `output_key` is simply
  // added. No-op if data services are unset or `output_key` is not in the catalog.
  void replaceCurve(const QString& source_key, const QString& output_key);

  // Drops curves whose source key is gone from the catalog (XY drops if either X
  // or Y source is gone), keeps the rest. One replot; returns whether anything
  // changed. Symmetric with IObjectViewer::revalidateObjects.
  bool revalidate();

  void setZoomRectangle(QRectF rect, bool emit_signal);
  [[nodiscard]] bool isZoomLinkEnabled() const noexcept;
  void setTrackerEnabled(bool enabled);
  [[nodiscard]] bool trackerEnabled() const noexcept;
  void setTrackerParameter(CurveTracker::Parameter parameter);
  // The tracker's current display level (line only / line + value / + name).
  [[nodiscard]] CurveTracker::Parameter trackerParameter() const noexcept;
  // Whether the playback tracker's floating value box is currently visible
  // (always hidden in line-only mode).
  [[nodiscard]] bool trackerValueBoxVisible() const noexcept;
  // Sets (or clears, when nullopt) a blue reference line. While set, the red
  // playback tracker renders values as deltas from this X. No-op on XY plots.
  void setReferenceLine(std::optional<double> reference_x_sec);
  // Mouse-hover inspector. Independent from the playback tracker_; toggling
  // this off does not hide the playback red line.
  void setShowPoints(bool show);
  [[nodiscard]] bool showPoints() const noexcept;
  // Enable/disable the canvas right-click context menu (off for read-only
  // previews, e.g. the Filter Editor's preview plot). Default on.
  void setContextMenuEnabled(bool enabled) noexcept {
    context_menu_enabled_ = enabled;
  }
  [[nodiscard]] QString stateId() const;
  void setStateId(QString id);
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& plot_element, bool autozoom = true);

  // Reads back the style currently applied to a Qwt curve (combining its
  // QwtPlotCurve::CurveStyle and the Inverted attribute) as a CurveStyle.
  [[nodiscard]] static CurveStyle qwtStyleToCurveStyle(const QwtPlotCurve* curve);

 public slots:
  void zoomOut(bool emit_signal = true);
  void onZoomOutHorizontalTriggered(bool emit_signal = true);
  void onZoomOutVerticalTriggered(bool emit_signal = true);
  void setTrackerPosition(double display_time_sec);
  void onChangeCurveColor(const QString& curve_name, QColor new_color);
  // Per-curve mutators (vs the plot-wide setLineWidth / overrideCurvesStyle).
  void setCurveLineWidth(const QString& curve_name, double width);
  void setCurveStyle(const QString& curve_name, CurveStyle style);
  void setCurveVisible(const QString& curve_name, bool visible);
  void removeAllCurves() override;

 signals:
  void rectChanged(PlotWidget* modified, QRectF rect);
  void undoableChange();
  void trackerMoved(QPointF point);
  void curvesDropped();
  void statusMessageRequested(QString message);
  void splitHorizontal();
  void splitVertical();
  void curveColorChanged(QString curve_name, QColor color);
  // "Apply Filter…" was chosen for this plot's curves. The host opens the Filter
  // Editor panel (chart-area takeover) scoped to `sources` and, on Apply, calls
  // `origin->replaceCurve()` so the filtered output replaces each source in place.
  void filterEditorRequested(std::vector<CurveDescriptor> sources, PlotWidget* origin);

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override;

 private slots:
  void onExternallyResized(const QRectF& rect);
  void onDragEnterEvent(QDragEnterEvent* event);
  void onDragLeaveEvent(QDragLeaveEvent* event);
  void onDropEvent(QDropEvent* event);

 private:
  // No-op when show_points_ is false.
  void showPointValues(QPoint paint_point);
  enum class DragMode { kNone, kCurves, kNewXY };

  struct DragInfo {
    DragMode mode = DragMode::kNone;
    QStringList curves;
  };

  void buildActions();
  void canvasContextMenuTriggered(const QPoint& pos);
  // Open the Filter Editor scoped to this plot's curves; on Save, add the
  // resulting filtered curve(s) to this plot.
  void launchFilterEditor();
  void setAxisScale(QwtAxisId axis_id, double min, double max);
  // The representative per-dataset display offset (in seconds) for this plot's
  // time axis: the offset of the first datastore-backed curve's dataset. Used to
  // convert the saved X-axis range between display-relative and absolute time at
  // the layout save/load boundary (xmlSaveState/xmlLoadState). Returns 0 when
  // there is no session or no datastore-backed curve — then display == absolute.
  [[nodiscard]] double displayOffsetSeconds() const;
  void reconnectDataSignals();
  [[nodiscard]] QStringList decodeCurveDrop(const QMimeData* mime_data, const QString& format) const;
  [[nodiscard]] bool allCurvesKnown(const QStringList& curves) const;
  [[nodiscard]] static QString lineWidthToString(LineWidth width);
  [[nodiscard]] static LineWidth lineWidthFromString(QString value);
  [[nodiscard]] static QString curveStyleToString(CurveStyle style);
  [[nodiscard]] static CurveStyle curveStyleFromString(QString value);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QMetaObject::Connection samples_ingested_connection_;
  QMetaObject::Connection dataset_replace_connection_;
  QMetaObject::Connection display_offset_connection_;
  DragInfo dragging_;
  CurveTracker* tracker_ = nullptr;
  CurveTracker* reference_tracker_ = nullptr;
  bool tracker_enabled_ = true;
  bool show_points_ = true;
  QwtPlotMarker* show_point_marker_ = nullptr;
  QwtPlotMarker* show_point_text_ = nullptr;
  // Used to skip replot when the mouse drifts but the snapped sample is unchanged.
  QPointF show_point_last_pos_;
  QString show_point_last_text_;
  QString state_id_;

  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_remove_all_curves_ = nullptr;
  QAction* action_zoom_out_ = nullptr;
  QAction* action_zoom_out_horizontal_ = nullptr;
  QAction* action_zoom_out_vertical_ = nullptr;
  bool context_menu_enabled_ = true;
};

}  // namespace PJ
