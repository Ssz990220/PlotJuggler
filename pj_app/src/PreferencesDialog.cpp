#include "PreferencesDialog.h"

#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSvgRenderer>

#include "MainWindow.h"
#include "Theme.h"
#include "ui/ToggleSwitch.h"
#include "ui_PreferencesDialog.h"

namespace PJ {

namespace {

// The toggle's track is gray (off) ↔ blue (on); the baked `#3D3D3D`
// fill on the sun/moon SVGs reads as muddy dark-gray-on-blue. Recolor
// to pure white at load time so the glyphs pop against either track
// tone. Used only here — keep it local rather than promoting a helper.
QIcon LoadWhiteFillIcon(const QString& resource_path) {
  QFile file(resource_path);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    return {};
  }
  QByteArray svg = file.readAll();
  svg.replace("#3D3D3D", "#FFFFFF");
  QSvgRenderer renderer(svg);
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  return QIcon(QPixmap::fromImage(image));
}

}  // namespace

PreferencesDialog::PreferencesDialog(Theme& theme, QWidget* parent)
    : Dialog(parent), ui_(new Ui::PreferencesDialog), theme_(theme), original_theme_(theme.currentTheme()) {
  setDialogTitle(tr("Preferences"));
  // The Dialog content area already has its own (zero-margin) layout, so
  // we instantiate the .ui onto a child body widget rather than setupUi(this).
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

  // Toggle: thumb-left = light, thumb-right = dark. The icon visible
  // in the un-covered slot represents the *destination* state — moon
  // when in light mode (click to go dark), sun when in dark mode.
  // Mapping: thumb-right (checked = true) → light theme, sun
  // visible in the left slot. thumb-left (checked = false) → dark
  // theme, moon visible in the right slot. "ON" reads as the
  // bright/active state.
  //
  // Icons are forced to white so they read clearly against the
  // colored track (PJBlue when on, gray when off).
  ui_->themeToggle->setLeftIcon(LoadWhiteFillIcon(QStringLiteral(":/resources/svg/light_mode_light.svg")));
  ui_->themeToggle->setRightIcon(LoadWhiteFillIcon(QStringLiteral(":/resources/svg/dark_mode_light.svg")));
  // Snap the toggle to the active theme without animating — the
  // dialog opens with the thumb already at its correct endpoint,
  // not mid-slide from 0 to 1 across the first 180ms after open.
  ui_->themeToggle->setChecked(original_theme_ == QLatin1String("light"), /*animate=*/false);

  // One-shot lock around the whole click → animation → setTheme →
  // propagation cycle. Three pieces:
  //
  //   1. `clicked` fires from mouseReleaseEvent / keyPressEvent
  //      right after the animation starts, BEFORE any subsequent
  //      input can be delivered. Disabling here blocks every
  //      further click for the entire 180ms slide, so the toggle
  //      can't be "trilled" mid-animation.
  //
  //   2. `toggled` fires only on natural animation completion
  //      (ToggleSwitch ties it to QPropertyAnimation::finished),
  //      so setTheme runs exactly once per locked cycle at the
  //      settled state.
  //
  //   3. `stylesheetChanged` re-enables — but via Qt::QueuedConnection.
  //      The signal is emitted synchronously inside setTheme (deep
  //      in MainWindow::onThemeChanged, after qApp->setStyleSheet
  //      + applyIcons + every dependent widget's onStylesheetChanged
  //      runs). A direct re-enable here would land mid-cascade,
  //      while the rest of setTheme (qssChanged → apply_theme_chrome →
  //      another qApp->setStyleSheet) is still running. Queued
  //      posts to the event loop and only fires after the entire
  //      synchronous chain has unwound and the event loop has
  //      drained any queued repaint events. By that point the
  //      toggle's visual state and theme_.currentTheme() are
  //      guaranteed to match.
  if (auto* main_window = qobject_cast<MainWindow*>(parent)) {
    connect(
        main_window, &MainWindow::stylesheetChanged, this,
        [this](const QString&) { ui_->themeToggle->setEnabled(true); }, Qt::QueuedConnection);
  }
  connect(ui_->themeToggle, &ToggleSwitch::clicked, this, [this]() { ui_->themeToggle->setEnabled(false); });
  connect(ui_->themeToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
    theme_.setTheme(checked ? QStringLiteral("light") : QStringLiteral("dark"));
  });
  connect(this, &QDialog::rejected, this, [this]() { theme_.setTheme(original_theme_); });

  connect(ui_->buttonOk, &QPushButton::clicked, this, &QDialog::accept);
  connect(ui_->buttonCancel, &QPushButton::clicked, this, &QDialog::reject);
}

PreferencesDialog::~PreferencesDialog() {
  delete ui_;
}

}  // namespace PJ
