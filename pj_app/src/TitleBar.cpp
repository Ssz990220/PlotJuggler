#include "TitleBar.h"

#include <QAction>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include "pj_widgets/SvgUtil.h"
#include "ui/DiagnosticsPopup.h"
#include "ui_TitleBar.h"

namespace PJ {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent), ui_(new Ui::TitleBar) {
  // QSS `background:` only paints on a custom QWidget subclass when
  // WA_StyledBackground is set. Built-in widgets (QPushButton etc.) do
  // this internally; bare QWidget subclasses must opt in or the rule is
  // a no-op and the title bar shows whatever is painted behind it.
  setAttribute(Qt::WA_StyledBackground, true);

  ui_->setupUi(this);
  // Hard-pin so QMainWindow::setMenuWidget can't size us via sizeHint
  // and leave a ghost strip of titlebar-background gray below the
  // buttons.
  //
  // Height is 24 to match every other chrome row (Sources, Datasets,
  // Custom Series, Playback). QSS adds `border-bottom: 1px` *inside*
  // the widget's geometry, so the inner content rect is 23 px tall.
  // The buttons in `TitleBar.ui` are therefore sized 23×23 — filling
  // the inner content exactly, no centering asymmetry, same visual
  // rhythm as the 23×23 tab-bar buttons (which sit in a 23-px bar
  // followed by a separate 1-px separator → 24 total chrome).
  setFixedHeight(24);

  // Tag every popup with objectName="PJMenu" so the QMenu#PJMenu rule
  // in stylesheet_*.qss applies. The id+type selector outranks the
  // cascading `QWidget { background: transparent }` rule that otherwise
  // wins for popups when QSS is delivered via qApp->setStyleSheet.
  app_menu_ = new QMenu(this);
  layout_menu_ = new QMenu(this);
  extension_menu_ = new QMenu(this);
  for (QMenu* m : {app_menu_, layout_menu_, extension_menu_}) {
    m->setObjectName(QStringLiteral("PJMenu"));
  }
  diagnostics_popup_ = new DiagnosticsPopup(this);
  diagnostics_popup_->setObjectName(QStringLiteral("DiagnosticsPopup"));
  connect(diagnostics_popup_, &DiagnosticsPopup::diagnosticActivated, this, &TitleBar::diagnosticActivated);

  // Manual popup — never `QToolButton::setMenu(...)` — because Qt
  // paints a dropdown arrow inside any button that has a menu set, and
  // that glyph survives every `::menu-indicator` / `::menu-button` /
  // `::menu-arrow` QSS suppression attempt. Popping by hand means the
  // button doesn't "have" a menu from Qt's perspective — no arrow —
  // while click → popup → outside-click-dismiss is identical UX.
  //
  // The `align_right` flag right-aligns the menu's top-right corner
  // with the button's bottom-right corner. Necessary for buttons in
  // the right-side cluster of the title bar — left-aligning a menu
  // there would push it off the screen edge and force Qt to nudge it
  // back, breaking visual alignment with the button.
  //
  // Order matters: we call popup() *before* measuring. Menus whose
  // content is built lazily in `aboutToShow` (e.g. extension_menu_)
  // have a stale 0-width `sizeHint()` until popup() fires the signal
  // and populates them. Measuring beforehand was making the first
  // click open the menu in the wrong spot, then subsequent clicks
  // (with a now-correct cached size) land correctly — i.e. the
  // "sometimes right, sometimes left" symptom.
  auto wire_popup = [this](
                        QToolButton* button, QMenu* menu, bool align_right = false, bool match_button_width = false) {
    connect(button, &QToolButton::clicked, this, [button, menu, align_right, match_button_width]() {
      // Provisional position at the button's bottom-left. popup()
      // here triggers aboutToShow which populates dynamic content.
      const QPoint provisional = button->mapToGlobal(QPoint(0, button->height()));
      menu->popup(provisional);
      // Ensure the menu spans at least the button's width — used for
      // the app menu so the popup extends across both the logo and
      // the "PlotJuggler" text rather than collapsing to its widest
      // menu item alone.
      if (match_button_width && menu->width() < button->width()) {
        menu->setMinimumWidth(button->width());
      }
      // Now the menu has its real, populated width. Snap to the
      // requested edge using width() (live size), not sizeHint().
      if (align_right) {
        const int dx = button->width() - menu->width();
        menu->move(button->mapToGlobal(QPoint(dx, button->height())));
      } else {
        // QMenu::popup() applies an auto-shift for tool-button popups;
        // pin back to the provisional point we asked for.
        menu->move(provisional);
      }
    });
  };
  wire_popup(ui_->appIcon, app_menu_, /*align_right=*/false, /*match_button_width=*/true);
  wire_popup(ui_->buttonLayout, layout_menu_, /*align_right=*/true);
  wire_popup(ui_->buttonExtension, extension_menu_, /*align_right=*/true);

  // Bell flash: 5-s single-shot timer flips the icon back to its
  // default glyph after the most recent diagnostic. Restarted on each
  // new record (see onDiagnosticRecorded) so a flurry of logs keeps the
  // active icon visible until the stream pauses.
  bell_idle_timer_ = new QTimer(this);
  bell_idle_timer_->setSingleShot(true);
  bell_idle_timer_->setInterval(5000);
  connect(bell_idle_timer_, &QTimer::timeout, this, [this]() {
    bell_active_ = false;
    ui_->buttonNotifications->setIcon(LoadSvg(":/resources/svg/alarm-bell.svg", currentTheme()));
  });

  applyIcons(currentTheme());
  connect(ui_->buttonNotifications, &QToolButton::clicked, this, [this]() {
    diagnostics_popup_->showAt(ui_->buttonNotifications);
    emit notificationsClicked();
  });
  connect(ui_->buttonMinimize, &QToolButton::clicked, this, [this]() {
    if (auto* w = window()) {
      w->showMinimized();
    }
  });
  connect(ui_->buttonPreferences, &QToolButton::clicked, this, &TitleBar::preferencesClicked);
  connect(ui_->buttonMaximize, &QToolButton::clicked, this, &TitleBar::onMaximizeClicked);
  connect(ui_->buttonClose, &QToolButton::clicked, this, [this]() {
    if (auto* w = window()) {
      w->close();
    }
  });
}

TitleBar::~TitleBar() {
  delete ui_;
}

QMenu* TitleBar::appMenu() const {
  return app_menu_;
}

QMenu* TitleBar::layoutMenu() const {
  return layout_menu_;
}

QMenu* TitleBar::extensionMenu() const {
  return extension_menu_;
}

void TitleBar::setDiagnosticHistory(DiagnosticHistory* history) {
  if (diagnostic_history_ != nullptr) {
    disconnect(diagnostic_history_, nullptr, this, nullptr);
  }
  diagnostic_history_ = history;
  diagnostics_popup_->setHistory(history);
  if (diagnostic_history_ == nullptr) {
    return;
  }
  connect(diagnostic_history_, &DiagnosticHistory::recorded, this, &TitleBar::onDiagnosticRecorded);
}

void TitleBar::onDiagnosticRecorded(const DiagnosticRecord& /*r*/) {
  // Flip to the "Notifications Active" icon for 5 s. Restarting the
  // timer on each new record means a steady stream of logs keeps the
  // active glyph showing until the stream pauses for a full interval.
  bell_active_ = true;
  ui_->buttonNotifications->setIcon(LoadSvg(":/resources/svg/alarm-bell-active.svg", currentTheme()));
  bell_idle_timer_->start();
}

void TitleBar::onStylesheetChanged(QString theme) {
  applyIcons(theme);
}

void TitleBar::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) {
    ui_->buttonMaximize->setToolTip(window()->isMaximized() ? tr("Restore") : tr("Maximize"));
  }
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !isOnMoveHandle(event->position().toPoint())) {
    QWidget::mousePressEvent(event);
    return;
  }
  if (auto* handle = window()->windowHandle()) {
    handle->startSystemMove();
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !isOnMoveHandle(event->position().toPoint())) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }
  onMaximizeClicked();
  event->accept();
}

void TitleBar::onMaximizeClicked() {
  auto* w = window();
  if (w == nullptr) {
    return;
  }
  if (w->isMaximized()) {
    w->showNormal();
  } else {
    w->showMaximized();
  }
}

bool TitleBar::isOnMoveHandle(const QPoint& pos) const {
  // Drag is allowed on raw bar background and on the non-interactive app
  // icon. Any click that lands on a tool button or its popup arrow goes
  // to the button.
  QWidget* hit = childAt(pos);
  return hit == nullptr || hit == ui_->appIcon;
}

void TitleBar::applyIcons(const QString& theme) {
  ui_->appIcon->setIcon(LoadSvg(":/resources/svg/plotjuggler.svg", theme));
  ui_->buttonExtension->setIcon(LoadSvg(":/resources/svg/extension.svg", theme));
  ui_->buttonNotifications->setIcon(
      LoadSvg(bell_active_ ? ":/resources/svg/alarm-bell-active.svg" : ":/resources/svg/alarm-bell.svg", theme));
  ui_->buttonLayout->setIcon(LoadSvg(":/resources/svg/mobile_layout.svg", theme));
  ui_->buttonPreferences->setIcon(LoadSvg(":/resources/svg/settings_cog_light.svg", theme));
  ui_->buttonMinimize->setIcon(LoadSvg(":/resources/svg/minimize.svg", theme));
  ui_->buttonMaximize->setIcon(LoadSvg(":/resources/svg/maximize.svg", theme));
  ui_->buttonClose->setIcon(LoadSvg(":/resources/svg/close_windows_light.svg", theme));
}

}  // namespace PJ
