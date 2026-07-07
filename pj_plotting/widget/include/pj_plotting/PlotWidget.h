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
  // Adds an XY (scatter) curve pairing the x_name/y_name series. `alias` is the
  // curve's display title (legend + identity key); an empty alias falls back to an
  // auto "Y vs X" title. Style/width are inherited from the plot (Dots, because
  // entering XY mode defaults the plot style) — not forced per-curve.
  CurveInfo* addCurveXY(
      const QString& x_name, const QString& y_name, const QString& alias, QColor color = Qt::transparent);

  // Interactive XY creation: pops the XYCurveDialog (Swap + alias) for the two
  // series, then adds the curve with the chosen orientation/alias. Returns nullptr
  // if the user cancels. Used by the on-canvas drop and the placeholder-dock drop;
  // never on layout/undo load (that path passes the saved alias to addCurveXY).
  CurveInfo* createCurveXYInteractive(const QString& x_key, const QString& y_key);

  // Add (or look up, if already present) the curve described by a layout `<curve>`
  // element and apply its saved style (color, line_width, style, visible). Picks
  // time-series vs XY from the plot's mode + the element's curve_x/curve_y attrs,
  // and is idempotent — re-applying the same element rebinds in place rather than
  // duplicating. This is the per-curve half of xmlLoadState, exposed so progressive
  // layout restore can bind a curve later (once its topic finishes loading) WITHOUT
  // re-running xmlLoadState, whose remove-pass would drop the already-live curves.
  // Returns the curve, or null if the element's key attribute is empty/unresolvable.
  CurveInfo* applyCurveElement(const QDomElement& curve_element);

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

  // Re-frame to the viewport stashed by the last xmlLoadState, converting any
  // absolute-time X with the display offset in effect NOW. xmlLoadState applies it
  // once, but during a PROGRESSIVE restore the catalog is still empty then (offset
  // 0), so the conversion is wrong until the dataset binds — progressive restore
  // calls this again per curve-bind and at drain to frame the plot to its final
  // (layout-saved) window up front and keep it pinned while data streams in, instead
  // of auto-fitting to the partial data. Falls back to zoomOut() when there is no
  // usable saved viewport (fresh load, or a degenerate/missing <range>). clear_after
  // drops the stash (pass true on the final drain pass).
  void applySavedViewportOrZoom(bool clear_after = false);

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
  // A dropped catalog key named an ADVERTISED (not-yet-subscribed) placeholder
  // topic rather than resolved data — dropping it must not fabricate a curve
  // (there is no storage id yet). The shell resolves demand + a pending bind
  // (see pj_app's TopicDemandController) instead of this widget.
  void placeholderCurveDropped(QString catalog_key);

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override;

 private slots:
  // Copies this plot's XML state to the widget clipboard.
  void copyWidgetToClipboard();
  // Replaces this plot's state from compatible clipboard XML.
  void pasteWidgetFromClipboard();
  void onExternallyResized(const QRectF& rect);
  void onDragEnterEvent(QDragEnterEvent* event);
  void onDragLeaveEvent(QDragLeaveEvent* event);
  void onDropEvent(QDropEvent* event);

 private:
  // Vertical-only auto-fit triggered when a curve is added or removed, gated by
  // the Preferences::auto_zoom_plots setting. Rescales the Y axis to the data
  // over the current X window only — the shared time axis (and thus sibling
  // plots) is never touched. No-op during layout restore (the saved range wins),
  // when empty, and for XY plots (no shared time axis).
  void autoZoomPlotVertically();

  // No-op when show_points_ is false.
  void showPointValues(QPoint paint_point);
  enum class DragMode { kNone, kCurves, kNewXY };

  struct DragInfo {
    DragMode mode = DragMode::kNone;
    QStringList curves;
  };

  void buildActions();
  void canvasContextMenuTriggered(const QPoint& pos);
  // True when the clipboard contains a plot XML payload.
  [[nodiscard]] bool canPasteWidgetFromClipboard() const;
  // Adds current opaque curve keys to copied XML for same-session paste.
  void stampClipboardCurveKeys(QDomElement& plot_element) const;
  // Resolves copied stable topic/field paths to this session's curve keys.
  void rebindClipboardCurveKeys(QDomElement& plot_element) const;
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
  // Drop the cached display offset on every bound DatastoreCurveAdapter whose
  // dataset matches `only` (or on all bound adapters when nullopt), so the next
  // paint re-maps the curve's X into the new frame. Returns whether any adapter
  // matched. PointSeriesXY ignores display offset (plan §12) and is skipped. The
  // caller owns the post-action (re-fit vs replot-at-current-zoom).
  bool invalidateAdapterOffsets(std::optional<DatasetId> only = std::nullopt);
  [[nodiscard]] QStringList decodeCurveDrop(const QMimeData* mime_data, const QString& format) const;
  [[nodiscard]] bool allCurvesKnown(const QStringList& curves) const;
  // Time-series drop gate (curveslist/add_curve): a curve name is droppable when
  // it resolves to real data OR names a scalar-shaped advertised placeholder
  // (kNone classification) — both are legitimate drop targets even though only
  // the former can materialize a curve immediately (onDropEvent routes the
  // latter to placeholderCurveDropped instead of addCurve). Unlike
  // allCurvesKnown (real curves only), used by the XY gesture, which has no
  // placeholder analogue.
  [[nodiscard]] bool allCurvesDroppable(const QStringList& curves) const;
  // True iff `name` is a scalar-shaped (kNone) advertised placeholder — i.e. a
  // droppable name allCurvesKnown would reject.
  [[nodiscard]] bool isPlaceholderCurveName(const QString& name) const;
  [[nodiscard]] static QString lineWidthToString(LineWidth width);
  [[nodiscard]] static LineWidth lineWidthFromString(QString value);
  // Maps a raw pen-width pixel value (older layouts' per-curve width) to the
  // closest LineWidth, tolerating both the raw {1.0,1.5,2.0,3.0} and the
  // lineWidthValue()-scaled forms.
  [[nodiscard]] static LineWidth lineWidthFromPixels(double pixels);
  [[nodiscard]] static QString curveStyleToString(CurveStyle style);
  [[nodiscard]] static CurveStyle curveStyleFromString(QString value);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QMetaObject::Connection samples_ingested_connection_;
  QMetaObject::Connection dataset_replace_connection_;
  QMetaObject::Connection display_offset_connection_;          // global "Use time offset" frame
  QMetaObject::Connection display_offset_dataset_connection_;  // per-source Timeline drag
  DragInfo dragging_;
  CurveTracker* tracker_ = nullptr;
  CurveTracker* reference_tracker_ = nullptr;
  bool tracker_enabled_ = true;
  bool show_points_ = true;
  bool loading_state_ = false;
  QwtPlotMarker* show_point_marker_ = nullptr;
  QwtPlotMarker* show_point_text_ = nullptr;
  // Used to skip replot when the mouse drifts but the snapped sample is unchanged.
  QPointF show_point_last_pos_;
  QString show_point_last_text_;
  QString state_id_;

  // Viewport stashed by xmlLoadState so progressive restore can re-apply it once the
  // per-dataset display offset is known (see applySavedViewportOrZoom). X bounds are
  // ABSOLUTE seconds for a time axis (converted to display on apply) or raw values for
  // an XY plot; Y (bottom/top) is raw. Unset means "no saved range" -> auto-fit.
  struct SavedViewport {
    double bottom = 0.0;
    double top = 0.0;
    double left = 0.0;
    double right = 0.0;
  };
  std::optional<SavedViewport> saved_viewport_;

  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_remove_all_curves_ = nullptr;
  QAction* action_zoom_out_ = nullptr;
  QAction* action_zoom_out_horizontal_ = nullptr;
  QAction* action_zoom_out_vertical_ = nullptr;
  bool context_menu_enabled_ = true;
};

}  // namespace PJ
