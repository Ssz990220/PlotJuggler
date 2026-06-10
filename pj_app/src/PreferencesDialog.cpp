// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesDialog.h"

#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QToolButton>
#include <QVBoxLayout>

#include "MainWindow.h"
#include "PreferencesNavRow.h"
#include "Theme.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ToggleSwitch.h"
#include "ui_PreferencesDialog.h"

namespace PJ {

namespace {

// First-launch defaults for the chrome-metric scrubbers. Mirrored from
// MainWindow.cpp's k*Default constants — kept in sync by hand because
// the reset button has to match the codepath that runs when QSettings
// has no saved value yet.
constexpr int kDefaultIconSize = 24;
constexpr int kDefaultIconPadding = 4;
constexpr int kDefaultLayoutPadding = 2;
constexpr int kDefaultLayoutSpacing = 2;

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
    : Dialog(parent),
      ui_(new Ui::PreferencesDialog),
      theme_(theme),
      original_theme_(theme.currentTheme()),
      original_metrics_(
          qobject_cast<MainWindow*>(parent) != nullptr ? qobject_cast<MainWindow*>(parent)->chromeMetrics()
                                                       : ChromeMetrics{}) {
  setDialogTitle(tr("Preferences"));
  // The Dialog content area already has its own (zero-margin) layout, so
  // we instantiate the .ui onto a child body widget rather than setupUi(this).
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);
  // Sensible default: room for the 160 px nav column, a comfortable page
  // body, and a few rows of category placeholders. Dialog is resizable
  // (edge-drag handled by the base class) so the user can grow / shrink.
  resize(560, 400);

  // Populate the left-hand nav column. Each row is a click-target that
  // switches pagesStack to the matching index. The trailing stretch in
  // navLayout (added in .ui) pushes the rows to the top.
  auto* nav_layout = qobject_cast<QVBoxLayout*>(ui_->navContainer->layout());
  const std::array<std::pair<QString, int>, 4> nav_entries{{
      {tr("Appearance"), 0},
      {tr("Plotting"), 1},
      {tr("Streaming"), 2},
      {tr("Scripting"), 3},
  }};
  nav_rows_.reserve(nav_entries.size());
  for (const auto& [label, index] : nav_entries) {
    auto* row = new PreferencesNavRow(label, ui_->navContainer);
    nav_layout->insertWidget(static_cast<int>(nav_rows_.size()), row);
    nav_rows_.push_back(row);
    connect(row, &PreferencesNavRow::clicked, this, [this, row, index]() {
      for (auto* other : nav_rows_) {
        other->setSelected(other == row);
      }
      ui_->pagesStack->setCurrentIndex(index);
    });
  }
  if (!nav_rows_.empty()) {
    nav_rows_.front()->setSelected(true);
    ui_->pagesStack->setCurrentIndex(0);
  }

  // Icon-size + icon-padding scrubbers. Ranges match the spec and the
  // clamps inside MainWindow::setIconSize / setIconPadding. Initial
  // values come from MainWindow so the dialog reflects the running
  // app's current state. Live preview: each valueChanged tick pushes
  // straight through MainWindow's setter (which clamps, persists, and
  // emits iconMetricsChanged) so the running app resizes in real time
  // while the user scrubs. On Cancel we restore the snapshot.
  auto* main_window = qobject_cast<MainWindow*>(parent);
  ui_->iconSizeScrubber->setRange(12, 48);
  ui_->iconSizeScrubber->setSingleStep(1);
  ui_->iconSizeScrubber->setSuffix(QStringLiteral(" px"));
  ui_->iconSizeScrubber->setValue(original_metrics_.icon_size);

  ui_->iconPaddingScrubber->setRange(0, 32);
  ui_->iconPaddingScrubber->setSingleStep(1);
  ui_->iconPaddingScrubber->setSuffix(QStringLiteral(" px"));
  ui_->iconPaddingScrubber->setValue(original_metrics_.icon_padding);

  ui_->layoutPaddingScrubber->setRange(0, 16);
  ui_->layoutPaddingScrubber->setSingleStep(1);
  ui_->layoutPaddingScrubber->setSuffix(QStringLiteral(" px"));
  ui_->layoutPaddingScrubber->setValue(original_metrics_.layout_padding);

  ui_->layoutSpacingScrubber->setRange(0, 16);
  ui_->layoutSpacingScrubber->setSingleStep(1);
  ui_->layoutSpacingScrubber->setSuffix(QStringLiteral(" px"));
  ui_->layoutSpacingScrubber->setValue(original_metrics_.layout_spacing);

  if (main_window != nullptr) {
    connect(ui_->iconSizeScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconSize);
    connect(ui_->iconPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconPadding);
    connect(ui_->layoutPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutPadding);
    connect(ui_->layoutSpacingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutSpacing);
  }

  // Reset-to-defaults button. Snaps each scrubber back to the
  // first-launch defaults; the scrubbers' valueChanged signals
  // already feed MainWindow's setters, so the running app live-
  // previews the reset and Cancel still reverts to the dialog's
  // open-time snapshot.
  ui_->buttonResetDefaults->setIcon(LoadSvg(":/resources/svg/restore_page.svg", theme_.currentTheme()));
  connect(ui_->buttonResetDefaults, &QToolButton::clicked, this, [this]() {
    ui_->iconSizeScrubber->setValue(kDefaultIconSize);
    ui_->iconPaddingScrubber->setValue(kDefaultIconPadding);
    ui_->layoutPaddingScrubber->setValue(kDefaultLayoutPadding);
    ui_->layoutSpacingScrubber->setValue(kDefaultLayoutSpacing);
  });

  // Toggle: thumb-left = light, thumb-right = dark. The icon visible
  // in the un-covered slot represents the *destination* state — moon
  // when in light mode (click to go dark), sun when in dark mode.
  // Mapping: thumb-right (checked = true) → light theme, sun
  // visible in the left slot. thumb-left (checked = false) → dark
  // theme, moon visible in the right slot. "ON" reads as the
  // bright/active state.
  //
  // Icons are forced to white so they read clearly against the
  // colored track (blue when on, gray when off).
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
  if (main_window != nullptr) {
    connect(
        main_window, &MainWindow::stylesheetChanged, this,
        [this](const QString&) { ui_->themeToggle->setEnabled(true); }, Qt::QueuedConnection);
  }
  connect(ui_->themeToggle, &ToggleSwitch::clicked, this, [this]() { ui_->themeToggle->setEnabled(false); });
  connect(ui_->themeToggle, &ToggleSwitch::toggled, this, [this](bool checked) {
    theme_.setTheme(checked ? QStringLiteral("light") : QStringLiteral("dark"));
  });
  connect(this, &QDialog::rejected, this, [this, main_window]() {
    theme_.setTheme(original_theme_);
    if (main_window != nullptr) {
      main_window->setIconSize(original_metrics_.icon_size);
      main_window->setIconPadding(original_metrics_.icon_padding);
      main_window->setLayoutPadding(original_metrics_.layout_padding);
      main_window->setLayoutSpacing(original_metrics_.layout_spacing);
    }
  });

  connect(ui_->buttonOk, &QPushButton::clicked, this, &QDialog::accept);
  connect(ui_->buttonCancel, &QPushButton::clicked, this, &QDialog::reject);
}

PreferencesDialog::~PreferencesDialog() {
  delete ui_;
}

}  // namespace PJ
