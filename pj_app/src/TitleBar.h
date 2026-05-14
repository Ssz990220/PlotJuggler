#pragma once

#include <QString>
#include <QWidget>

#include "pj_runtime/DiagnosticHistory.h"

class QMenu;
class QMouseEvent;
class QTimer;

namespace Ui {
class TitleBar;
}

namespace PJ {

class DiagnosticsPopup;

// Custom title bar for a frameless QMainWindow. Hosts the App / Tools /
// Help popup buttons on the left and minimize / maximize / close on the
// right. Empty regions act as the system-move handle; double-click on
// empty regions toggles maximize.
class TitleBar : public QWidget {
  Q_OBJECT
 public:
  explicit TitleBar(QWidget* parent = nullptr);
  ~TitleBar() override;

  // The TitleBar owns three popup menus that callers populate directly.
  // appMenu() is the consolidated menu attached to the PlotJuggler icon
  // (top-left), holding what used to be split across App / Tools / Help.
  // layoutMenu() is the layout-button popup. extensionMenu() is the
  // installed-extensions dropdown attached to buttonExtension; the
  // caller subscribes to its aboutToShow to rebuild the entry list
  // lazily.
  [[nodiscard]] QMenu* appMenu() const;
  [[nodiscard]] QMenu* layoutMenu() const;
  [[nodiscard]] QMenu* extensionMenu() const;

  // Wire the title-bar bell + diagnostics popup to a DiagnosticHistory.
  // The history is the single source of truth for diagnostics; the bell
  // label tracks the latest record and the popup observes the buffer.
  void setDiagnosticHistory(DiagnosticHistory* history);

 signals:
  // Forwarded from buttonNotifications. Kept for callers that still want
  // the raw click event in addition to the built-in popup behaviour.
  void notificationsClicked();

  // Emitted when the user clicks the cog button in the title bar.
  // MainWindow handles it by opening the Preferences dialog.
  void preferencesClicked();

  // Emitted when the user clicks a card in the diagnostics popup. The
  // owner (MainWindow) opens a frameless detail dialog in response.
  void diagnosticActivated(const DiagnosticRecord& item);

 public slots:
  void onStylesheetChanged(QString theme);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void changeEvent(QEvent* event) override;

 private slots:
  void onDiagnosticRecorded(const DiagnosticRecord& r);

 private:
  void applyIcons(const QString& theme);
  void onMaximizeClicked();
  [[nodiscard]] bool isOnMoveHandle(const QPoint& pos) const;

  Ui::TitleBar* ui_;
  QMenu* app_menu_ = nullptr;
  QMenu* layout_menu_ = nullptr;
  QMenu* extension_menu_ = nullptr;
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
};

}  // namespace PJ
