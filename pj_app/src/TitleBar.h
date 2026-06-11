#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QWidget>

#include "pj_runtime/DiagnosticHistory.h"
#include "pj_widgets/ChromeMetrics.h"

class QMenu;
class QMouseEvent;
class QTimer;

namespace Ui {
class TitleBar;
}

namespace PJ {

class DiagnosticsPopup;

// Custom title bar for a frameless QMainWindow. Hosts the app icon and
// a traditional QMenuBar (File / Toolbox / Help) on the left, and the
// relocated panel toggles, the notification bell and minimize /
// maximize / close on the right. Empty regions act as the system-move
// handle; double-click on empty regions toggles maximize.
class TitleBar : public QWidget {
  Q_OBJECT
 public:
  explicit TitleBar(QWidget* parent = nullptr);
  ~TitleBar() override;

  // The TitleBar owns the QMenuBar's three popup menus; MainWindow
  // populates them. fileMenu() holds layout load/save + marketplace +
  // preferences + quit. toolboxMenu() and the Help "Installed
  // Extensions" submenu are rebuilt lazily by the caller on
  // aboutToShow, so they track the live extension catalog.
  [[nodiscard]] QMenu* fileMenu() const;
  [[nodiscard]] QMenu* toolboxMenu() const;
  [[nodiscard]] QMenu* helpMenu() const;

  // Inserts a widget into the right-side cluster, between the
  // notification bell and the window controls. Repeated calls append
  // left-to-right. Used by the shell to relocate the three
  // panel-toggle buttons created by TabbedPlotWidget.
  void addRightClusterWidget(QWidget* widget);

  // Wire the title-bar bell + diagnostics popup to a DiagnosticHistory.
  // The history is the single source of truth for diagnostics; the bell
  // label tracks the latest record and the popup observes the buffer.
  void setDiagnosticHistory(DiagnosticHistory* history);

 signals:
  // Forwarded from buttonNotifications. Kept for callers that still want
  // the raw click event in addition to the built-in popup behaviour.
  void notificationsClicked();

  // Emitted when the user clicks a card in the diagnostics popup. The
  // owner (MainWindow) opens a frameless detail dialog in response.
  void diagnosticActivated(const DiagnosticRecord& item);

 public slots:
  void onStylesheetChanged(QString theme);

  // Rebinds Chrome metrics from MainWindow. Re-sizes the title bar to
  // (icon_size + icon_padding) + 2 * layout_padding tall, sets each
  // chrome button to (icon_size + icon_padding) square, pushes
  // layout_padding as contentsMargins on the horizontal layout, and
  // layout_spacing as the gap between adjacent chrome buttons.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void changeEvent(QEvent* event) override;

 private slots:
  void onDiagnosticRecorded(const DiagnosticRecord& r);

 private:
  void applyIcons(const QString& theme);
  void applyIconMetrics();
  void onMaximizeClicked();
  [[nodiscard]] bool isOnMoveHandle(const QPoint& pos) const;

  Ui::TitleBar* ui_;
  QMenu* file_menu_ = nullptr;
  QMenu* toolbox_menu_ = nullptr;
  QMenu* help_menu_ = nullptr;
  DiagnosticsPopup* diagnostics_popup_ = nullptr;
  DiagnosticHistory* diagnostic_history_ = nullptr;
  // Single-shot timer that flips the bell icon back to its default
  // glyph 5 s after the most recent diagnostic. Restarted on each new
  // record, so a flurry of logs keeps the active icon visible until
  // the stream pauses.
  QTimer* bell_idle_timer_ = nullptr;
  // True while the bell is showing the "Notifications Active" icon
  // (timer running). Stored so applyIcons() can pick the right SVG on
  // theme change without consulting the timer.
  bool bell_active_ = false;

  // Chrome metrics broadcast from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
