// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/marketplace_window.hpp"

#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_detail_dialog.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "ui_marketplace_window.h"

namespace PJ {

static constexpr const char* kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry"
    "/refs/heads/development/registry.json";

namespace {

// Event filter that turns a click on a frameless dialog's title bar
// into a system move. Mirrors what MarketplaceWindow does in its own
// mousePressEvent, but exposed as a filter so it can serve transient
// dialogs spun up inline (e.g. the Marketplace Settings popup).
class FramelessTitleBarDragFilter : public QObject {
 public:
  FramelessTitleBarDragFilter(QDialog* dlg, QWidget* title_bar, QLabel* title_label, QObject* parent = nullptr)
      : QObject(parent), dlg_(dlg), title_bar_(title_bar), title_label_(title_label) {}

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == title_bar_ && event->type() == QEvent::MouseButtonPress) {
      auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() == Qt::LeftButton) {
        // Only treat clicks on bare title-bar area or the label as drag
        // starts — clicks on the close button etc. fall through.
        QWidget* hit = title_bar_->childAt(me->position().toPoint());
        if ((hit == nullptr || hit == title_label_) && dlg_->windowHandle() != nullptr) {
          dlg_->windowHandle()->startSystemMove();
          return true;
        }
      }
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  QDialog* dlg_;
  QWidget* title_bar_;
  QLabel* title_label_;
};

bool installedStatesEqual(const QMap<QString, InstalledExtension>& lhs, const QMap<QString, InstalledExtension>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (auto it = lhs.cbegin(); it != lhs.cend(); ++it) {
    const auto rhs_it = rhs.find(it.key());
    if (rhs_it == rhs.cend()) {
      return false;
    }
    const InstalledExtension& a = it.value();
    const InstalledExtension& b = rhs_it.value();
    if (a.id != b.id || a.version != b.version || a.enabled != b.enabled) {
      return false;
    }
  }
  return true;
}

}  // namespace

MarketplaceWindow::MarketplaceWindow(const QUrl& registry_url, QWidget* parent)
    : QDialog(parent), ui_(new Ui::MarketplaceWindow) {
  download_mgr_ = new DownloadManager(this);
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = new ExtensionManager(
      download_mgr_, PlatformUtils::extensionsDir(), PlatformUtils::pendingDir(), /*sink*/ {}, this);
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

  installChrome();
  setupUi();
  setupSignals();
  updateDiagnosticsButton();
  showLatestDiagnostic();
  // applyPendingUninstalls/applyPendingInstalls already ran in ExtensionManager::initComponents().
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent)
    : QDialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

  installChrome();
  setupUi();
  setupSignals();
  updateDiagnosticsButton();
  showLatestDiagnostic();
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::MarketplaceWindow(
    ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
    QWidget* parent)
    : QDialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  initial_snapshot_provided_ = true;
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

  installChrome();
  setupUi();
  setupSignals();
  ext_mgr_->setInstalledExtensions(installed);
  updateDiagnosticsButton();
  showLatestDiagnostic();
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::~MarketplaceWindow() {
  delete ui_;
}

void MarketplaceWindow::installChrome() {
  // Frameless + no system shadow so the WM-drawn chrome doesn't overrule
  // the app's title-bar style. Mirrors what Dialog does in pj_app —
  // duplicated here because pj_marketplace_ui can't depend on pj_app
  // without a circular link.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::NoDropShadowWindowHint, true);
  setAttribute(Qt::WA_StyledBackground, true);
  setWindowTitle(tr("PlotJuggler Marketplace"));

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  dialog_title_bar_ = new QWidget(this);
  dialog_title_bar_->setObjectName("dialogTitleBar");
  dialog_title_bar_->setFixedHeight(24);
  auto* tb_layout = new QHBoxLayout(dialog_title_bar_);
  tb_layout->setContentsMargins(10, 0, 0, 0);
  tb_layout->setSpacing(0);

  dialog_title_label_ = new QLabel(tr("PlotJuggler Marketplace"), dialog_title_bar_);
  dialog_title_label_->setObjectName("dialogTitleLabel");

  auto* close_btn = new QToolButton(dialog_title_bar_);
  close_btn->setObjectName("buttonClose");
  close_btn->setFixedSize(23, 23);
  close_btn->setAutoRaise(true);
  close_btn->setFocusPolicy(Qt::NoFocus);
  close_btn->setIconSize(QSize(20, 20));
  const bool dark_theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString() !=
                          QStringLiteral("light");
  close_btn->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/close_windows_dark.svg")
                 : QStringLiteral(":/resources/svg/close_windows_light.svg")));
  connect(close_btn, &QToolButton::clicked, this, &QDialog::reject);

  tb_layout->addWidget(dialog_title_label_);
  tb_layout->addStretch();
  tb_layout->addWidget(close_btn);

  auto* body = new QWidget(this);
  body->setObjectName("dialogContent");
  ui_->setupUi(body);

  outer->addWidget(dialog_title_bar_);
  outer->addWidget(body);
}

void MarketplaceWindow::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && dialog_title_bar_ != nullptr &&
      dialog_title_bar_->geometry().contains(event->position().toPoint())) {
    QWidget* hit = dialog_title_bar_->childAt(dialog_title_bar_->mapFrom(this, event->position().toPoint()));
    if (hit == nullptr || hit == dialog_title_label_) {
      if (auto* h = windowHandle()) {
        h->startSystemMove();
        event->accept();
        return;
      }
    }
  }
  QDialog::mousePressEvent(event);
}

// ─── UI Setup ────────────────────────────────────────────────────────────────

void MarketplaceWindow::setupUi() {
  ui_->update_all_btn_->setFixedWidth(90);
  ui_->update_all_btn_->setEnabled(false);

  // The marketplace doesn't link pj_app_core, so it can't pipe icons
  // through LoadSvg's recolor. Pick the theme-appropriate variant
  // directly from the resource bundle.
  const bool dark_theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString() !=
                          QStringLiteral("light");
  ui_->settings_btn_->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/settings_cog_dark.svg")
                 : QStringLiteral(":/resources/svg/settings_cog_light.svg")));
  ui_->refresh_btn_->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/reload_dark.svg")
                 : QStringLiteral(":/resources/svg/reload_light.svg")));
  // Magnifying glass leading the search field, matching the curve list filter.
  ui_->search_edit_->addAction(
      QIcon(
          dark_theme ? QStringLiteral(":/resources/svg/search_dark.svg")
                     : QStringLiteral(":/resources/svg/search_light.svg")),
      QLineEdit::LeadingPosition);
  // Trailing clear-text "X" — Qt's built-in clearButtonEnabled paints
  // a stock SP_LineEditClearButton that ignores our theme, so we wire
  // up our own QAction with the same close glyph used by the dialog
  // chrome. Hidden when the field is empty.
  auto* clear_search_action = ui_->search_edit_->addAction(
      QIcon(
          dark_theme ? QStringLiteral(":/resources/svg/close_windows_dark.svg")
                     : QStringLiteral(":/resources/svg/close_windows_light.svg")),
      QLineEdit::TrailingPosition);
  clear_search_action->setVisible(false);
  connect(ui_->search_edit_, &QLineEdit::textChanged, clear_search_action, [clear_search_action](const QString& text) {
    clear_search_action->setVisible(!text.isEmpty());
  });
  connect(clear_search_action, &QAction::triggered, ui_->search_edit_, &QLineEdit::clear);

  // Scroll-area background comes from the central stylesheet
  // (#scroll_area_ rule binds it to ${dark_background}).

  ui_->category_combo_->addItem("All categories", "");
  ui_->category_combo_->addItem("Data Loader", "data_loader");
  ui_->category_combo_->addItem("Data Streamer", "data_streamer");
  ui_->category_combo_->addItem("Message Parser", "parser");
  ui_->category_combo_->addItem("Toolbox", "toolbox");

  connect(ui_->search_edit_, &QLineEdit::textChanged, this, &MarketplaceWindow::onSearchChanged);
  connect(
      ui_->category_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      &MarketplaceWindow::onCategoryChanged);
  connect(ui_->refresh_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onRefreshClicked);
  connect(ui_->update_all_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onUpdateAllClicked);
  connect(ui_->settings_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onSettingsClicked);
  connect(ui_->diagnostics_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onDiagnosticsClicked);
}

// ─── Signal wiring ───────────────────────────────────────────────────────────

void MarketplaceWindow::setupSignals() {
  // RegistryManager
  connect(registry_mgr_, &RegistryManager::fetchStarted, this, [this]() { setStatus("Loading registry..."); });

  connect(registry_mgr_, &RegistryManager::fetchFinished, this, [this](bool success) {
    if (!success) {
      setStatus("Failed to load registry", true);
      return;
    }
    // A successful refresh is a strong "things are working" signal; let it
    // override any old sticky error so progress messages aren't suppressed.
    clearStickyStatus();
    extensions_ = registry_mgr_->compatibleExtensions(PlatformUtils::currentPlatform());
    applyFilters();
    setStatus("Ready — " + QString::number(extensions_.size()) + " extensions loaded");
  });

  connect(ext_mgr_, &ExtensionManager::installPendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    populateCards();
    setStatus(QString("Extension %1 staged — will be active after restart").arg(id));
    processInstallQueue();
  });

  connect(ext_mgr_, &ExtensionManager::uninstallPendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    populateCards();
    setStatus(QString("Extension %1 staged — will be uninstalled after restart").arg(id));
  });

  connect(registry_mgr_, &RegistryManager::fetchError, this, [this](const QString& error) {
    setStatus("Registry error: " + error, true);
  });

  // ExtensionManager
  connect(ext_mgr_, &ExtensionManager::installStarted, this, [this](const QString& id) {
    ui_->progress_bar_->setValue(0);
    ui_->progress_bar_->setRange(0, 100);
    ui_->progress_bar_->setVisible(true);
    for (const auto& ext : extensions_) {
      if (ext.id == id) {
        setStatus("Installing " + ext.name + "...");
        break;
      }
    }
  });

  connect(ext_mgr_, &ExtensionManager::installProgress, this, [this](const QString& /*id*/, int percent) {
    ui_->progress_bar_->setValue(percent);
  });

  connect(ext_mgr_, &ExtensionManager::installFinished, this, [this](const QString& id, bool success) {
    ui_->progress_bar_->setVisible(false);
    if (success) {
      installations_changed_ = true;
    }
    populateCards();
    if (success) {
      status_error_sticky_ = false;
      for (const auto& ext : extensions_) {
        if (ext.id == id) {
          setStatus("Installed " + ext.name + " v" + ext.version);
          break;
        }
      }
    }
    // On failure the status was already set by installError — do not overwrite it.
    processInstallQueue();
  });

  connect(ext_mgr_, &ExtensionManager::installError, this, [this](const QString& /*id*/, const QString& error) {
    ui_->progress_bar_->setVisible(false);
    setStatus("Installation failed: " + error, true);
    // Queue advance lives in installFinished only — installError + installFinished both
    // fire from emitInstallFailure, so advancing here would double-pop the queue.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallFinished, this, [this](const QString& id, bool success) {
    if (success) {
      status_error_sticky_ = false;
      installations_changed_ = true;
      populateCards();
      for (const auto& ext : extensions_) {
        if (ext.id == id) {
          setStatus("Uninstalled " + ext.name);
          break;
        }
      }
    }
    // On failure the status was already set by uninstallError — do not overwrite it.
  });

  connect(ext_mgr_, &ExtensionManager::uninstallError, this, [this](const QString& /*id*/, const QString& error) {
    setStatus("Uninstall failed: " + error, true);
  });

  connect(
      ext_mgr_, &ExtensionManager::diagnosticReported, this,
      [this](const QString& /*id*/, const QString& message, bool is_error) {
        updateDiagnosticsButton();
        if (is_error) {
          setStatus("Marketplace diagnostic: " + message, true);
        }
      });
}

// ─── Cards Population ─────────────────────────────────────────────────────────

void MarketplaceWindow::populateCards() {
  while (ui_->cards_layout_->count() > 1) {
    delete ui_->cards_layout_->takeAt(0)->widget();
  }

  const auto installed = ext_mgr_->installedExtensions();
  bool has_updatable = false;
  for (const Extension& ext : filtered_) {
    const QString ext_id = ext.id;

    auto* card = new QFrame(ui_->cards_container);
    card->setFrameShape(QFrame::NoFrame);
    card->setProperty("ext_id", ext_id);
    card->setToolTip(ext.description);
    card->setCursor(Qt::PointingHandCursor);
    card->setObjectName("extCard");
    card->installEventFilter(this);
    // Card surface (theme-relative ${marketplace_card_bg}) and hover
    // are wired in resources/stylesheet_*.qss.

    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(10, 8, 10, 8);
    card_layout->setSpacing(4);

    auto* top_row = new QHBoxLayout();

    auto* name_lbl = new QLabel(ext.name, card);
    QFont f = name_lbl->font();
    f.setBold(true);
    name_lbl->setFont(f);

    const bool has_update = ext_mgr_->hasUpdate(ext);
    const bool has_newer_local = ext_mgr_->hasNewerInstalledVersion(ext);
    if (has_update) {
      has_updatable = true;
    }

    QString version_text = ext.version;
    if (installed.contains(ext.id)) {
      version_text = installed[ext.id].version;
      if (has_update) {
        version_text += " \u2192 " + ext.version;
      } else if (has_newer_local) {
        version_text += " \u2191 " + ext.version;
      }
    }
    auto* version_lbl = new QLabel(version_text, card);
    // Text colour comes from the QFrame#extCard QLabel rule.

    auto* btn_box = new QHBoxLayout();
    btn_box->setSpacing(6);

    // Per-state action button / status badge. Object name selects the
    // matching #extButton* / #extBadge* rule in resources/stylesheet_*.qss.
    if (ext_mgr_->hasPendingInstall(ext.id) || ext_mgr_->hasPendingUninstall(ext.id)) {
      auto* badge = new QPushButton("Needs Restart", card);
      badge->setObjectName("extBadgeNeedsRestart");
      badge->setFixedWidth(90);
      badge->setEnabled(false);
      btn_box->addWidget(badge);
    } else if (has_update) {
      auto* btn = new QPushButton("Update \u2B06", card);
      btn->setObjectName("extButtonUpdate");
      btn->setFixedWidth(90);
      connect(btn, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
      btn_box->addWidget(btn);
    } else if (has_newer_local) {
      auto* badge = new QPushButton("Local newer", card);
      badge->setObjectName("extBadgeLocalNewer");
      badge->setFixedWidth(90);
      badge->setEnabled(false);
      btn_box->addWidget(badge);
    } else if (installed.contains(ext.id)) {
      auto* badge = new QPushButton("Installed", card);
      badge->setObjectName("extBadgeInstalled");
      badge->setFixedWidth(90);
      badge->setEnabled(false);
      btn_box->addWidget(badge);
    } else {
      auto* btn = new QPushButton("Install", card);
      btn->setObjectName("extButtonInstall");
      btn->setFixedWidth(90);
      connect(btn, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
      btn_box->addWidget(btn);
    }

    top_row->addWidget(name_lbl);
    top_row->addStretch();
    top_row->addWidget(version_lbl);
    card_layout->addLayout(top_row);

    auto* bottom_row = new QHBoxLayout();
    auto* desc_lbl = new QLabel(card);
    desc_lbl->setObjectName("extCardDescription");
    QFontMetrics fm(desc_lbl->font());
    desc_lbl->setText(fm.elidedText(ext.description, Qt::ElideRight, 400));
    bottom_row->addWidget(desc_lbl);
    bottom_row->addStretch();
    bottom_row->addLayout(btn_box);
    card_layout->addLayout(bottom_row);

    ui_->cards_layout_->insertWidget(ui_->cards_layout_->count() - 1, card);
  }

  ui_->update_all_btn_->setEnabled(has_updatable && update_queue_.isEmpty());
}

// ─── Event Filter (double-click on card) ─────────────────────────────────────

bool MarketplaceWindow::eventFilter(QObject* obj, QEvent* event) {
  if (event->type() == QEvent::MouseButtonDblClick) {
    const QString ext_id = static_cast<QFrame*>(obj)->property("ext_id").toString();
    if (!ext_id.isEmpty()) {
      openDetail(ext_id);
    }
    return true;
  }
  return QDialog::eventFilter(obj, event);
}

void MarketplaceWindow::openDetail(const QString& ext_id) {
  for (const auto& ext : filtered_) {
    if (ext.id != ext_id) {
      continue;
    }
    const auto installed = ext_mgr_->installedExtensions();
    const QString installed_version = installed.contains(ext_id) ? installed[ext_id].version : QString{};
    ExtensionDetailDialog dlg(ext, installed_version, this);
    connect(&dlg, &ExtensionDetailDialog::installRequested, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
    connect(
        &dlg, &ExtensionDetailDialog::uninstallRequested, this, [this, ext_id]() { onUninstallButtonClicked(ext_id); });
    dlg.exec();
    return;
  }
}

// ─── Filtering ────────────────────────────────────────────────────────────────

void MarketplaceWindow::applyFilters() {
  const QString search = ui_->search_edit_->text().toLower();
  const QString category = ui_->category_combo_->currentData().toString();

  filtered_.clear();
  for (const auto& ext : extensions_) {
    if (!category.isEmpty() && ext.category != category) {
      continue;
    }
    if (!search.isEmpty()) {
      bool match = ext.name.toLower().contains(search) || ext.description.toLower().contains(search);
      if (!match) {
        for (const auto& tag : ext.tags) {
          if (tag.toLower().contains(search)) {
            match = true;
            break;
          }
        }
      }
      if (!match) {
        continue;
      }
    }
    filtered_.append(ext);
  }

  populateCards();
  setStatus(QString::number(filtered_.size()) + " of " + QString::number(extensions_.size()) + " extensions shown");
}

void MarketplaceWindow::setStatus(const QString& msg, bool is_error) {
  if (!is_error && status_error_sticky_) {
    return;
  }
  status_error_sticky_ = is_error;
  ui_->status_label_->setText(msg);
  // The error tone is keyed off objectName via the
  // QLabel#marketplaceStatusError rule in resources/stylesheet_*.qss.
  // Clearing the objectName restores the inherited default text style.
  ui_->status_label_->setObjectName(is_error ? QStringLiteral("marketplaceStatusError") : QString{});
  ui_->status_label_->style()->unpolish(ui_->status_label_);
  ui_->status_label_->style()->polish(ui_->status_label_);
}

void MarketplaceWindow::clearStickyStatus() {
  status_error_sticky_ = false;
}

void MarketplaceWindow::showLatestDiagnostic() {
  const QList<ExtensionDiagnostic> diagnostics = ext_mgr_->diagnostics();
  if (diagnostics.isEmpty()) {
    return;
  }
  const ExtensionDiagnostic& diagnostic = diagnostics.back();
  setStatus("Marketplace diagnostic: " + diagnostic.message, diagnostic.is_error);
}

void MarketplaceWindow::updateDiagnosticsButton() {
  const int count = ext_mgr_->diagnostics().size();
  ui_->diagnostics_btn_->setVisible(count > 0);
  ui_->diagnostics_btn_->setText(count > 1 ? QString("Details (%1)").arg(count) : "Details");
}

// ─── Slots ────────────────────────────────────────────────────────────────────

void MarketplaceWindow::onSearchChanged(const QString& /*text*/) {
  applyFilters();
}
void MarketplaceWindow::onCategoryChanged(int /*index*/) {
  applyFilters();
}

void MarketplaceWindow::onRefreshClicked() {
  clearStickyStatus();
  setStatus("Refreshing...");
  const auto before = ext_mgr_->installedExtensions();
  ext_mgr_->refreshInstalledFromDisk();
  if (!installedStatesEqual(ext_mgr_->installedExtensions(), before)) {
    installations_changed_ = true;
  }
  populateCards();
  registry_mgr_->fetchRegistry(registry_url_);
}

void MarketplaceWindow::showEvent(QShowEvent* event) {
  if (ext_mgr_ != nullptr) {
    if (initial_snapshot_provided_) {
      initial_snapshot_provided_ = false;
      populateCards();
    } else {
      const auto before = ext_mgr_->installedExtensions();
      ext_mgr_->refreshInstalledFromDisk();
      if (!installedStatesEqual(ext_mgr_->installedExtensions(), before)) {
        installations_changed_ = true;
        populateCards();
      }
    }
    updateDiagnosticsButton();
    showLatestDiagnostic();
  }
  QDialog::showEvent(event);
}

void MarketplaceWindow::onSettingsClicked() {
  // Frameless chrome matching the marketplace window itself —
  // duplicated inline because pj_marketplace can't depend on pj_app's
  // Dialog without a circular link.
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Marketplace Settings"));
  dlg.setWindowFlag(Qt::FramelessWindowHint, true);
  dlg.setWindowFlag(Qt::NoDropShadowWindowHint, true);
  dlg.setAttribute(Qt::WA_StyledBackground, true);
  dlg.setMinimumWidth(480);

  auto* outer = new QVBoxLayout(&dlg);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* title_bar = new QWidget(&dlg);
  title_bar->setObjectName("dialogTitleBar");
  title_bar->setFixedHeight(24);
  auto* tb_layout = new QHBoxLayout(title_bar);
  tb_layout->setContentsMargins(10, 0, 0, 0);
  tb_layout->setSpacing(0);

  auto* title_label = new QLabel(tr("Marketplace Settings"), title_bar);
  title_label->setObjectName("dialogTitleLabel");

  auto* close_btn = new QToolButton(title_bar);
  close_btn->setObjectName("buttonClose");
  close_btn->setFixedSize(23, 23);
  close_btn->setAutoRaise(true);
  close_btn->setFocusPolicy(Qt::NoFocus);
  close_btn->setIconSize(QSize(20, 20));
  const bool dark_theme = QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString() !=
                          QStringLiteral("light");
  close_btn->setIcon(QIcon(
      dark_theme ? QStringLiteral(":/resources/svg/close_windows_dark.svg")
                 : QStringLiteral(":/resources/svg/close_windows_light.svg")));
  connect(close_btn, &QToolButton::clicked, &dlg, &QDialog::reject);

  tb_layout->addWidget(title_label);
  tb_layout->addStretch();
  tb_layout->addWidget(close_btn);

  // Drag the dialog by clicking the bare title-bar area or the label.
  auto* drag_filter = new FramelessTitleBarDragFilter(&dlg, title_bar, title_label, &dlg);
  title_bar->installEventFilter(drag_filter);

  auto* body = new QWidget(&dlg);
  body->setObjectName("dialogContent");
  auto* layout = new QFormLayout(body);

  auto* url_edit = new QLineEdit(registry_url_.toString(), body);
  url_edit->setPlaceholderText(kDefaultRegistryUrl);
  layout->addRow(tr("Registry URL:"), url_edit);

  auto* extensions_path = new QLineEdit(ext_mgr_->extensionsDir(), body);
  extensions_path->setReadOnly(true);
  // No inline stylesheet — the global QLineEdit QSS gives this the
  // themed background. The previous `palette(window)` override pulled
  // the Fusion window-role colour, which doesn't match the new
  // dark_background-based dialog body.
  layout->addRow(tr("Extensions path:"), extensions_path);

  auto* button_layout = new QHBoxLayout;
  button_layout->addStretch();
  auto* cancel_button = new QPushButton(tr("Cancel"), body);
  cancel_button->setProperty("destructive", true);
  auto* ok_button = new QPushButton(tr("OK"), body);
  button_layout->addWidget(cancel_button);
  button_layout->addWidget(ok_button);
  layout->addRow(button_layout);

  connect(ok_button, &QPushButton::clicked, &dlg, &QDialog::accept);
  connect(cancel_button, &QPushButton::clicked, &dlg, &QDialog::reject);

  outer->addWidget(title_bar);
  outer->addWidget(body);

  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const QString text = url_edit->text().trimmed();
  const QUrl new_url(text);
  if (text.isEmpty() || !new_url.isValid() ||
      (new_url.scheme() != "http" && new_url.scheme() != "https" && new_url.scheme() != "file")) {
    QMessageBox::warning(
        this, "Invalid registry URL",
        QString("\"%1\" is not a valid http(s) or file URL. The registry URL was not changed.").arg(text));
    return;
  }
  if (new_url == registry_url_) {
    return;
  }

  clearStickyStatus();
  registry_url_ = new_url;
  QSettings("PlotJuggler", "Marketplace").setValue("registry_url", registry_url_.toString());

  setStatus("Refreshing...");
  registry_mgr_->fetchRegistry(registry_url_);
}

void MarketplaceWindow::onActionButtonClicked(const QString& ext_id) {
  for (const auto& ext : filtered_) {
    if (ext.id != ext_id) {
      continue;
    }
    clearStickyStatus();
    if (ext_mgr_->hasUpdate(ext)) {
      ext_mgr_->update(ext);
    } else if (ext_mgr_->hasNewerInstalledVersion(ext)) {
      setStatus("Installed version is newer than registry version", true);
    } else if (!ext_mgr_->isInstalled(ext.id)) {
      ext_mgr_->install(ext);
    }
    return;
  }
}

void MarketplaceWindow::onUninstallButtonClicked(const QString& ext_id) {
  clearStickyStatus();
  ext_mgr_->uninstall(ext_id);
}

void MarketplaceWindow::onUpdateAllClicked() {
  clearStickyStatus();
  update_queue_.clear();
  for (const auto& ext : filtered_) {
    if (ext_mgr_->hasUpdate(ext)) {
      update_queue_.append(ext);
    }
  }
  if (update_queue_.isEmpty()) {
    return;
  }
  ui_->update_all_btn_->setEnabled(false);
  setStatus("Updating " + QString::number(update_queue_.size()) + " extensions...");
  processInstallQueue();
}

void MarketplaceWindow::onDiagnosticsClicked() {
  QDialog dlg(this);
  dlg.setWindowTitle("Marketplace Diagnostics");
  dlg.resize(640, 360);

  auto* layout = new QVBoxLayout(&dlg);
  auto* text = new QPlainTextEdit(&dlg);
  text->setReadOnly(true);

  QStringList lines;
  for (const ExtensionDiagnostic& diagnostic : ext_mgr_->diagnostics()) {
    const QString level = diagnostic.is_error ? "ERROR" : "INFO";
    const QString id = diagnostic.id.isEmpty() ? "-" : diagnostic.id;
    lines.append(QString("[%1] %2 %3: %4")
                     .arg(diagnostic.timestamp.toLocalTime().toString(Qt::ISODate), level, id, diagnostic.message));
  }
  text->setPlainText(lines.isEmpty() ? "No diagnostics." : lines.join('\n'));
  layout->addWidget(text);

  auto* close_row = new QHBoxLayout;
  close_row->addStretch();
  auto* close_button = new QPushButton(tr("Close"), &dlg);
  close_row->addWidget(close_button);
  connect(close_button, &QPushButton::clicked, &dlg, &QDialog::reject);
  layout->addLayout(close_row);
  dlg.exec();
}

void MarketplaceWindow::processInstallQueue() {
  if (update_queue_.isEmpty()) {
    return;
  }
  ext_mgr_->update(update_queue_.takeFirst());
}

}  // namespace PJ
