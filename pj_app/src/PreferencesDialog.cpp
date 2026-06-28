// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PreferencesDialog.h"

#include <QAbstractItemModel>
#include <QBrush>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QToolButton>
#include <QVBoxLayout>

#include "DebugMode.h"
#include "MainWindow.h"
#include "PreferencesNavRow.h"
#include "Theme.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/SvgButton.h"
#include "pj_widgets/ThemeColors.h"
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
QIcon loadWhiteFillIcon(const QString& resource_path) {
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
      ui_(new Ui::PreferencesContent),
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
  // Free the dialog to resize down. The base Dialog's top-level layout uses Qt's
  // default SetDefaultConstraint, which on every activation forces the window's
  // minimum size up to the layout's computed minimum — and with the Appearance
  // page's Fixed-height scrubber rows that minimum is tall enough to block
  // shrinking (and to override a smaller restored/explicit size). SetNoConstraint
  // stops the layout from imposing that minimum (children still fill via their
  // own layouts); the explicit minimum below is then the only floor.
  if (auto* root = layout()) {
    root->setSizeConstraint(QLayout::SetNoConstraint);
  }
  setMinimumSize(420, 300);
  // Restore the user's last dialog size if they resized it before; otherwise a
  // sensible default (room for the 160 px nav column + a comfortable page body).
  // The dialog is resizable (edge-drag handled by the base class); geometry is
  // saved on close (see the destructor).
  {
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("Preferences::dialog_geometry")).toByteArray();
    // A stale/corrupt blob (Qt upgrade, truncated .ini) makes restoreGeometry
    // return false and apply nothing — fall back to the default size rather than
    // opening off-screen or at 0x0.
    if (geometry.isEmpty() || !restoreGeometry(geometry)) {
      resize(560, 400);
    }
  }

  // Populate the left-hand nav column. Each row is a click-target that
  // switches pagesStack to the matching index. The trailing stretch in
  // navLayout (added in .ui) pushes the rows to the top.
  auto* nav_layout = qobject_cast<QVBoxLayout*>(ui_->navContainer->layout());
  const std::array<std::pair<QString, int>, 5> nav_entries{{
      {tr("Appearance"), 0},
      {tr("Plotting"), 1},
      {tr("Scene 2D"), 2},
      {tr("Scene 3D"), 3},
      {tr("Plugins"), 4},
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

  // Developer-only chrome-metric scrubbers (icon/layout sizing) ship hidden;
  // --debug-mode reveals them. They sit in a spanning row of the Appearance
  // form, so toggle the whole row (setRowVisible) to reclaim its vertical space
  // when hidden — hiding only the widget would leave the row's gap.
  if (auto* appearance_form = qobject_cast<QFormLayout*>(ui_->pageAppearance->layout())) {
    int row = -1;
    QFormLayout::ItemRole role{};
    appearance_form->getWidgetPosition(ui_->debugChromeWidget, &row, &role);
    if (row >= 0) {
      appearance_form->setRowVisible(row, isDebugMode());
    }
  }

  // Scene 3D page is a placeholder for now — the asset-folder controls are
  // shown but disabled until the feature lands.
  ui_->pageScene3D->setEnabled(false);

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

  // Value preferences seeded together (one QSettings read pass) and committed on
  // OK below. The scoped block keeps `settings` from shadowing the same-named
  // local in the accept/reject handlers (which run later as slots):
  //  - Float precision (Appearance): 1-6 decimals; its readers (plot tooltips,
  //    the curve tracker, the curve-list value column) re-read the key on their
  //    next redraw.
  //  - Curve-colour sequence (Plotting): global = one continuous colour sequence
  //    across all plots; per plot = the sequence restarts within each plot.
  //  - OpenGL (Appearance): default on; PlotWidgetBase reads the key when a plot
  //    is constructed (applies to newly created plots). --disable-opengl can
  //    force it off for a session without touching this saved value.
  ui_->scrubberFloatPrecision->setRange(1, 6);
  ui_->scrubberFloatPrecision->setSingleStep(1);
  ui_->curveColorMode->setOptions(tr("global"), tr("per plot"));
  {
    QSettings settings;
    ui_->scrubberFloatPrecision->setValue(settings.value(QStringLiteral("Preferences::precision"), 3).toInt());
    ui_->curveColorMode->setSelectedIndex(
        settings.value(QStringLiteral("Preferences::curve_color_global"), true).toBool() ? 0 : 1);
    ui_->openglToggle->setChecked(
        settings.value(QStringLiteral("Preferences::use_opengl"), true).toBool(),
        /*animate=*/false);
  }

  if (main_window != nullptr) {
    connect(ui_->iconSizeScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconSize);
    connect(ui_->iconPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setIconPadding);
    connect(ui_->layoutPaddingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutPadding);
    connect(ui_->layoutSpacingScrubber, &IntScrubber::valueChanged, main_window, &MainWindow::setLayoutSpacing);
  }

  // Plugins page: the user-managed custom folder list (drag-reorderable, with
  // add/remove) plus the read-only built-in folders. Folders that do not exist
  // on disk render in red. The custom list persists on OK and applies on next
  // launch (no hot reload of extensions).
  if (main_window != nullptr) {
    auto paint_missing = [](QListWidget* list) {
      for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        const bool missing = !QDir(item->text()).exists();
        item->setForeground(missing ? QBrush(theme::kAccentError) : QBrush());
        item->setToolTip(missing ? tr("This folder does not exist.") : QString());
      }
    };

    ui_->listCustomPluginFolders->setDragDropMode(QAbstractItemView::InternalMove);
    ui_->listCustomPluginFolders->addItems(main_window->customPluginFolders());
    paint_missing(ui_->listCustomPluginFolders);
    // A drag-reorder re-serializes the items and drops their foreground brush, so
    // repaint the missing-folder tint after a move.
    connect(ui_->listCustomPluginFolders->model(), &QAbstractItemModel::rowsMoved, this, [this, paint_missing]() {
      paint_missing(ui_->listCustomPluginFolders);
    });

    ui_->listDefaultPluginFolders->addItems(main_window->builtinPluginFolders());
    ui_->listDefaultPluginFolders->setSelectionMode(QAbstractItemView::NoSelection);
    ui_->listDefaultPluginFolders->setFocusPolicy(Qt::NoFocus);
    paint_missing(ui_->listDefaultPluginFolders);

    // SvgButton re-tints itself on a theme change — no manual retint wiring.
    ui_->buttonAddPluginFolder->setIconPath(QStringLiteral(":/resources/svg/add.svg"));
    ui_->buttonAddPluginFolder->setExtent(26, 24);
    ui_->buttonRemovePluginFolder->setIconPath(QStringLiteral(":/resources/svg/trash.svg"));
    ui_->buttonRemovePluginFolder->setExtent(26, 24);
    ui_->buttonAddPluginFolder->setToolTip(tr("Add a plugin folder…"));
    ui_->buttonRemovePluginFolder->setToolTip(tr("Remove the selected folder"));
    connect(ui_->buttonAddPluginFolder, &QToolButton::clicked, this, [this, paint_missing]() {
      const QString dir = QFileDialog::getExistingDirectory(this, tr("Add plugin folder"));
      if (!dir.isEmpty()) {
        ui_->listCustomPluginFolders->addItem(dir);
        paint_missing(ui_->listCustomPluginFolders);
      }
    });
    connect(ui_->buttonRemovePluginFolder, &QToolButton::clicked, this, [this, paint_missing]() {
      qDeleteAll(ui_->listCustomPluginFolders->selectedItems());
      paint_missing(ui_->listCustomPluginFolders);
    });
  }

  // Reset-to-defaults button. Snaps each scrubber back to the
  // first-launch defaults; the scrubbers' valueChanged signals
  // already feed MainWindow's setters, so the running app live-
  // previews the reset and Cancel still reverts to the dialog's
  // open-time snapshot.
  // Same glyph as the timeline align-rail "reset all" button (restart_alt).
  // SvgButton re-tints itself on a theme change.
  ui_->buttonResetDefaults->setIconPath(QStringLiteral(":/resources/svg/restart_alt.svg"));
  ui_->buttonResetDefaults->setSize(SvgButton::Size::kDefault);
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
  // Larger than the compact 34x18 default so the sun/moon icons read clearly.
  ui_->themeToggle->setFixedSize(44, 24);
  // Icons are forced to white so they read clearly against the
  // colored track (blue when on, gray when off).
  ui_->themeToggle->setLeftIcon(loadWhiteFillIcon(QStringLiteral(":/resources/svg/light_mode_light.svg")));
  ui_->themeToggle->setRightIcon(loadWhiteFillIcon(QStringLiteral(":/resources/svg/dark_mode_light.svg")));
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

  // Plotting page: auto-zoom plots. When on, adding/removing a curve rescales
  // that plot's Y axis to fit (see PlotWidget::autoZoomPlotVertically). Seed
  // from the persisted preference (no animation — open at the settled
  // position); like the other plotting prefs this commits only on OK.
  {
    QSettings settings;
    ui_->autoZoomToggle->setChecked(
        settings.value(QStringLiteral("Preferences::auto_zoom_plots"), true).toBool(),
        /*animate=*/false);
  }
  connect(this, &QDialog::rejected, this, [this, main_window]() {
    theme_.setTheme(original_theme_);
    if (main_window != nullptr) {
      main_window->setIconSize(original_metrics_.icon_size);
      main_window->setIconPadding(original_metrics_.icon_padding);
      main_window->setLayoutPadding(original_metrics_.layout_padding);
      main_window->setLayoutSpacing(original_metrics_.layout_spacing);
    }
  });
  // The chrome setters above only apply live — commit to QSettings on OK. Cancel
  // restores the snapshot and never persisted, so the .ini keeps the originals.
  connect(this, &QDialog::accepted, this, [this, main_window]() {
    if (main_window != nullptr) {
      main_window->persistChromeMetrics();
      QStringList plugin_folders;
      for (int row = 0; row < ui_->listCustomPluginFolders->count(); ++row) {
        plugin_folders << ui_->listCustomPluginFolders->item(row)->text();
      }
      main_window->setCustomPluginFolders(plugin_folders);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("Preferences::precision"), ui_->scrubberFloatPrecision->value());
    settings.setValue(QStringLiteral("Preferences::use_opengl"), ui_->openglToggle->isChecked());
    settings.setValue(QStringLiteral("Preferences::curve_color_global"), ui_->curveColorMode->selectedIndex() == 0);
    settings.setValue(QStringLiteral("Preferences::auto_zoom_plots"), ui_->autoZoomToggle->isChecked());
  });

  connect(ui_->buttonOk, &QPushButton::clicked, this, &QDialog::accept);
  connect(ui_->buttonCancel, &QPushButton::clicked, this, &QDialog::reject);
}

PreferencesDialog::~PreferencesDialog() {
  // Remember the dialog size across launches regardless of OK/Cancel — window
  // size is a UI preference, not a settings change.
  QSettings settings;
  settings.setValue(QStringLiteral("Preferences::dialog_geometry"), saveGeometry());
  delete ui_;
}

}  // namespace PJ
