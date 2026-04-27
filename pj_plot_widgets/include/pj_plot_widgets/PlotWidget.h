#pragma once

#include <qwt_axis_id.h>

#include <QAction>
#include <QDomDocument>
#include <QDomElement>
#include <QMetaObject>
#include <QRectF>
#include <QStringList>

#include "pj_plot_widgets/PlotWidgetBase.h"

class QDragEnterEvent;
class QDragLeaveEvent;
class QDropEvent;
class QMimeData;

namespace PJ {

class CatalogModel;
class CurveTracker;
class SessionManager;

class PlotWidget : public PlotWidgetBase {
  Q_OBJECT
 public:
  explicit PlotWidget(SessionManager* session = nullptr, CatalogModel* catalog = nullptr, QWidget* parent = nullptr);
  ~PlotWidget() override;

  void setDataServices(SessionManager* session, CatalogModel* catalog);
  CurveInfo* addCurve(const QString& name, QColor color = Qt::transparent);
  CurveInfo* addCurveXY(const QString& x_name, const QString& y_name, QColor color = Qt::transparent);

  void setZoomRectangle(QRectF rect, bool emit_signal);
  [[nodiscard]] bool isZoomLinkEnabled() const noexcept;
  void setTrackerEnabled(bool enabled);
  [[nodiscard]] bool trackerEnabled() const noexcept;
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& plot_element, bool autozoom = true);

 public slots:
  void zoomOut(bool emit_signal = true);
  void onZoomOutHorizontalTriggered(bool emit_signal = true);
  void onZoomOutVerticalTriggered(bool emit_signal = true);
  void setTrackerPosition(double display_time_sec);
  void onChangeCurveColor(const QString& curve_name, QColor new_color);
  void removeAllCurves() override;

 signals:
  void rectChanged(PlotWidget* modified, QRectF rect);
  void undoableChange();
  void trackerMoved(QPointF point);
  void curvesDropped();
  void statusMessageRequested(QString message);
  void splitHorizontal();
  void splitVertical();

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override;

 private slots:
  void onExternallyResized(const QRectF& rect);
  void onDragEnterEvent(QDragEnterEvent* event);
  void onDragLeaveEvent(QDragLeaveEvent* event);
  void onDropEvent(QDropEvent* event);

 private:
  enum class DragMode { kNone, kCurves, kNewXY };

  struct DragInfo {
    DragMode mode = DragMode::kNone;
    QStringList curves;
  };

  void buildActions();
  void canvasContextMenuTriggered(const QPoint& pos);
  [[nodiscard]] CurveInfo* curveAtPosition(const QPoint& pos);
  void setAxisScale(QwtAxisId axis_id, double min, double max);
  void reconnectDataSignals();
  [[nodiscard]] QStringList decodeCurveDrop(const QMimeData* mime_data, const QString& format) const;
  [[nodiscard]] bool allCurvesKnown(const QStringList& curves) const;
  [[nodiscard]] static QString lineWidthToString(LineWidth width);
  [[nodiscard]] static LineWidth lineWidthFromString(QString value);

  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QMetaObject::Connection topics_committed_connection_;
  DragInfo dragging_;
  CurveTracker* tracker_ = nullptr;
  bool tracker_enabled_ = true;

  QAction* action_split_horizontal_ = nullptr;
  QAction* action_split_vertical_ = nullptr;
  QAction* action_remove_all_curves_ = nullptr;
  QAction* action_zoom_out_ = nullptr;
  QAction* action_zoom_out_horizontal_ = nullptr;
  QAction* action_zoom_out_vertical_ = nullptr;
  bool context_menu_enabled_ = true;
};

}  // namespace PJ
