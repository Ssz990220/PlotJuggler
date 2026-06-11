// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TitleBar.h"

#include <QAction>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
#include <array>

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
  // Height tracks the global icon-metrics setting: button height equals
  // icon_size + icon_padding, and the title-bar itself is +1 to leave
  // room for the QSS `border-bottom: 1px` that draws inside our
  // geometry. Initial values are the defaults; MainWindow re-pushes the
  // saved metrics via iconMetricsChanged after the connection is wired.
  applyIconMetrics();

  // Traditional menus, owned here so MainWindow can populate them.
  // objectName "PJMenu" keeps the existing QMenu#PJMenu QSS applying to
  // the popups (the id+type selector outranks the cascading
  // `QWidget { background: transparent }` rule that otherwise wins for
  // popups when QSS is delivered via qApp->setStyleSheet).
  // setNativeMenuBar(false) forces in-window rendering even on desktops
  // with a global menu bar — the menubar is part of our custom chrome,
  // not the platform's.
  file_menu_ = new QMenu(tr("&File"), this);
  toolbox_menu_ = new QMenu(tr("&Toolbox"), this);
  help_menu_ = new QMenu(tr("&Help"), this);
  ui_->menuBar->setNativeMenuBar(false);
  for (QMenu* menu : {file_menu_, toolbox_menu_, help_menu_}) {
    menu->setObjectName(QStringLiteral("PJMenu"));
    ui_->menuBar->addMenu(menu);
  }
  diagnostics_popup_ = new DiagnosticsPopup(this);
  diagnostics_popup_->setObjectName(QStringLiteral("DiagnosticsPopup"));
  connect(diagnostics_popup_, &DiagnosticsPopup::diagnosticActivated, this, &TitleBar::diagnosticActivated);

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

QMenu* TitleBar::fileMenu() const {
  return file_menu_;
}

QMenu* TitleBar::toolboxMenu() const {
  return toolbox_menu_;
}

QMenu* TitleBar::helpMenu() const {
  return help_menu_;
}

void TitleBar::addRightClusterWidget(QWidget* widget) {
  if (widget == nullptr) {
    return;
  }
  // Appends to the bell's group so the added widgets share its tight
  // intra-group spacing; the spacer + outer layout spacing keep the
  // group visually separate from the window controls.
  ui_->rightClusterLayout->addWidget(widget);
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

void TitleBar::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  applyIconMetrics();
}

void TitleBar::applyIconMetrics() {
  const int button_extent = chrome_metrics_.icon_size + chrome_metrics_.icon_padding;
  // Bar = button + 2 * layout_padding + 1 (the QSS bottom border draws
  // inside our geometry, so the inner content rect is bar_height - 1).
  const int bar_height = button_extent + (2 * chrome_metrics_.layout_padding) + 1;
  setMinimumHeight(bar_height);
  setMaximumHeight(bar_height);
  setFixedHeight(bar_height);
  if (auto* layout = ui_->horizontalLayout) {
    layout->setContentsMargins(
        chrome_metrics_.layout_padding, chrome_metrics_.layout_padding, chrome_metrics_.layout_padding,
        chrome_metrics_.layout_padding);
    layout->setSpacing(chrome_metrics_.layout_spacing);
  }

  // Square chrome buttons — fixed extent on both axes.
  const QSize icon_sz(chrome_metrics_.icon_size, chrome_metrics_.icon_size);
  const std::array<QToolButton*, 4> square_buttons{
      ui_->buttonNotifications, ui_->buttonMinimize, ui_->buttonMaximize, ui_->buttonClose};
  for (QToolButton* btn : square_buttons) {
    btn->setMinimumSize(button_extent, button_extent);
    btn->setMaximumSize(button_extent, button_extent);
    btn->setIconSize(icon_sz);
  }
  ui_->appIcon->setMinimumHeight(button_extent);
  ui_->appIcon->setMaximumHeight(button_extent);
  ui_->appIcon->setIconSize(icon_sz);
  // The menubar tracks the chrome-button height so its highlight rect
  // matches the buttons around it.
  ui_->menuBar->setFixedHeight(button_extent);
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
  ui_->buttonNotifications->setIcon(
      LoadSvg(bell_active_ ? ":/resources/svg/alarm-bell-active.svg" : ":/resources/svg/alarm-bell.svg", theme));
  ui_->buttonMinimize->setIcon(LoadSvg(":/resources/svg/minimize.svg", theme));
  ui_->buttonMaximize->setIcon(LoadSvg(":/resources/svg/maximize.svg", theme));
  ui_->buttonClose->setIcon(LoadSvg(":/resources/svg/close_windows_light.svg", theme));
}

}  // namespace PJ
