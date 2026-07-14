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
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_detail_dialog.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/MessageBox.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/Search.h"
#include "ui_marketplace_window.h"
using namespace Qt::StringLiterals;

namespace PJ {

static constexpr const char* kDefaultRegistryUrl =
    "https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry"
    "/refs/heads/development/registry.json";

namespace {

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
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  download_mgr_ = new DownloadManager(this);
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = new ExtensionManager(
      download_mgr_, PlatformUtils::extensionsDir(), PlatformUtils::pendingDir(), /*sink*/ {}, this);
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

  setupUi();
  setupSignals();
  updateDiagnosticsButton();
  showLatestDiagnostic();
  // applyPendingUninstalls/applyPendingInstalls already ran in ExtensionManager::initComponents().
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

  setupUi();
  setupSignals();
  updateDiagnosticsButton();
  showLatestDiagnostic();
  registry_mgr_->fetchRegistry(registry_url_);
}

MarketplaceWindow::MarketplaceWindow(
    ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
    QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  initial_snapshot_provided_ = true;
  QSettings settings("PlotJuggler", "Marketplace");
  const QString saved = settings.value("registry_url").toString();
  registry_url_ = saved.isEmpty() ? registry_url : QUrl(saved);

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

// ─── UI Setup ────────────────────────────────────────────────────────────────

void MarketplaceWindow::setupUi() {
  // Canonical chrome: build the .ui onto a child body under PJ::Dialog's title
  // bar (its content area already owns a zero-margin layout).
  setDialogTitle(tr("PlotJuggler Marketplace"));
  auto* body = new QWidget;
  ui_->setupUi(body);
  contentLayout()->addWidget(body);

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
  // The canonical Search provides the (self-retinting) magnifying glass and a
  // themed clear "x"; it sits on the toolbar surface, so use the standalone tone.
  ui_->search_edit_->setVariant(Search::Variant::kStandalone);

  // Scroll-area background comes from the central stylesheet
  // (#scroll_area_ rule binds it to ${dark_background}).

  ui_->category_combo_->addItem("All categories", "");
  ui_->category_combo_->addItem("Data Loader", "data_loader");
  ui_->category_combo_->addItem("Data Streamer", "data_stream");
  ui_->category_combo_->addItem("Message Parser", "message_parser");
  ui_->category_combo_->addItem("Toolbox", "toolbox");

  connect(ui_->search_edit_, &Search::textChanged, this, &MarketplaceWindow::onSearchChanged);
  connect(
      ui_->category_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      &MarketplaceWindow::onCategoryChanged);
  connect(ui_->refresh_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onRefreshClicked);
  connect(ui_->update_all_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onUpdateAllClicked);
  connect(ui_->settings_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onSettingsClicked);
  connect(ui_->diagnostics_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onDiagnosticsClicked);

  // Canonical overlay pill scrollbars for the extension list / detail scroll
  // areas just built (their ranges update live as the registry loads).
  attachPillScrollbars(this);
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
    // Staging finishes the active install just like installFinished does; clear
    // the busy marker so the card flips to "Needs Restart" (not a stuck
    // "Installing" badge) and processInstallQueue() can advance to the next item.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
    installations_changed_ = true;
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
    active_install_id_ = id;
    ui_->progress_bar_->setValue(0);
    ui_->progress_bar_->setRange(0, 100);
    ui_->progress_bar_->setVisible(true);
    showInstallProgress();
    populateCards();  // repaint so the active card shows the "Installing" badge
  });

  connect(ext_mgr_, &ExtensionManager::installProgress, this, [this](const QString& /*id*/, int percent) {
    ui_->progress_bar_->setValue(percent);
  });

  // Post-download phases (verifying, extracting) do not report byte-level
  // progress, so we flip the bar to indeterminate/busy mode and update the
  // status label with the current phase.
  connect(
      ext_mgr_, &ExtensionManager::installPhase, this, [this](const QString& /*id*/, DownloadManager::WorkPhase phase) {
        ui_->progress_bar_->setRange(0, 0);
        QString verb;
        switch (phase) {
          case DownloadManager::WorkPhase::Verifying:
            verb = u"Verifying"_s;
            break;
          case DownloadManager::WorkPhase::Extracting:
            verb = u"Extracting"_s;
            break;
        }
        showInstallProgress(verb);
      });

  connect(ext_mgr_, &ExtensionManager::installFinished, this, [this](const QString& id, bool success) {
    // Only clear the busy marker if this is the finish of the install we
    // actually started. A failure from a call that never reached
    // installStarted (rejected by an ExtensionManager guard, e.g.
    // unsupported platform) also emits installFinished with success=false;
    // in that case active_install_id_ still points at the install that IS
    // in flight and must stay set until it completes.
    if (id == active_install_id_) {
      active_install_id_.clear();
    }
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
    card_layout->setContentsMargins(
        theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable),
        theme::space(theme::Space::Comfortable), theme::space(theme::Space::Comfortable));
    card_layout->setSpacing(theme::space(theme::Space::Snug));

    auto* top_row = new QHBoxLayout();

    auto* name_lbl = new QLabel(ext.name, card);
    QFont f = name_lbl->font();
    f.setBold(true);
    name_lbl->setFont(f);

    const bool has_update = ext_mgr_->hasUpdate(ext);
    const bool has_newer_local = ext_mgr_->hasNewerInstalledVersion(ext);
    const bool pending = ext_mgr_->hasPendingInstall(ext.id) || ext_mgr_->hasPendingUninstall(ext.id);
    // A staged update keeps its old installed version until restart, so
    // hasUpdate() stays true — exclude already-pending extensions so "Update
    // All" doesn't stay enabled and re-stage what is already queued for restart.
    if (has_update && !pending) {
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
    btn_box->setSpacing(theme::space(theme::Space::Comfortable));

    // An install is in flight or queued for this extension (the active one, an
    // explicit click awaiting its turn, or an Update All entry). Show a disabled
    // "Installing" badge on all of them until the operation completes.
    const bool queued_for_update =
        std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext.id; });
    const bool installing = ext.id == active_install_id_ || pending_clicks_.contains(ext.id) || queued_for_update;

    // Per-state action button / status badge. Object name selects the
    // matching #extButton* / #extBadge* rule in resources/stylesheet_*.qss.
    if (installing) {
      auto* badge = new QPushButton("Installing", card);
      badge->setObjectName("extBadgeInstalling");
      badge->setFixedWidth(90);
      badge->setEnabled(false);
      btn_box->addWidget(badge);
    } else if (ext_mgr_->hasPendingInstall(ext.id) || ext_mgr_->hasPendingUninstall(ext.id)) {
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
  // Open the detail dialog on a double-click of a card. Consume ONLY when the
  // event is on a card (it carries an "ext_id"): the base PJ::Dialog installs
  // this object as an app-wide event filter (for resize-edge cursors), so this
  // override sees every widget's events — returning true unconditionally would
  // swallow the card's double-click at the window level before it propagates
  // down to the card, and no detail dialog would ever open.
  if (event->type() == QEvent::MouseButtonDblClick) {
    const QString ext_id = obj->property("ext_id").toString();
    if (!ext_id.isEmpty()) {
      openDetail(ext_id);
      return true;
    }
  }
  return Dialog::eventFilter(obj, event);
}

void MarketplaceWindow::openDetail(const QString& ext_id) {
  for (const auto& ext : filtered_) {
    if (ext.id != ext_id) {
      continue;
    }
    const auto installed = ext_mgr_->installedExtensions();
    const QString installed_version = installed.contains(ext_id) ? installed[ext_id].version : QString{};
    // Mirror the card's pending state so the dialog can't offer an action on an
    // install/update or uninstall that is already staged for the next restart.
    const bool needs_restart = ext_mgr_->hasPendingInstall(ext_id) || ext_mgr_->hasPendingUninstall(ext_id);
    ExtensionDetailDialog dlg(ext, installed_version, needs_restart, this);
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
  ui_->status_label_->setObjectName(is_error ? u"marketplaceStatusError"_s : QString{});
  ui_->status_label_->style()->unpolish(ui_->status_label_);
  ui_->status_label_->style()->polish(ui_->status_label_);
}

void MarketplaceWindow::clearStickyStatus() {
  status_error_sticky_ = false;
}

QString MarketplaceWindow::queueSuffix() const {
  const int queued = pending_clicks_.size() + update_queue_.size();
  if (queued == 0) {
    return {};
  }
  return u"  ·  "_s + QString::number(queued) + u" queued"_s;
}

void MarketplaceWindow::showInstallProgress(const QString& verb) {
  if (active_install_id_.isEmpty()) {
    return;
  }
  QString name = active_install_id_;
  for (const auto& ext : extensions_) {
    if (ext.id == active_install_id_) {
      name = ext.name;
      break;
    }
  }
  setStatus(verb + u" "_s + name + u"…"_s + queueSuffix());
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
  Dialog::showEvent(event);
}

void MarketplaceWindow::onSettingsClicked() {
  Dialog dlg(this);
  dlg.setDialogTitle(tr("Marketplace Settings"));
  dlg.setMinimumWidth(480);

  auto* body = new QWidget;
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

  dlg.contentLayout()->addWidget(body);

  if (dlg.exec() != QDialog::Accepted) {
    return;
  }

  const QString text = url_edit->text().trimmed();
  const QUrl new_url(text);
  if (text.isEmpty() || !new_url.isValid() ||
      (new_url.scheme() != "http" && new_url.scheme() != "https" && new_url.scheme() != "file")) {
    MessageBox::warning(
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
  // If another install/update is already in flight, queue this click and let
  // processInstallQueue() dispatch it when the current one completes.
  // Otherwise ExtensionManager::install() would reject with
  // "Install of X is already in progress" — its single-install-at-a-time
  // model is intentional, we just hide it behind a queue at the UI layer.
  if (!active_install_id_.isEmpty()) {
    if (ext_id == active_install_id_ || pending_clicks_.contains(ext_id)) {
      return;  // deduplicate — either it IS the running one or already queued
    }
    pending_clicks_.append(ext_id);
    showInstallProgress();  // keep the active install visible; reflect the new queue depth
    populateCards();        // repaint so the queued card shows the "Installing" badge
    return;
  }

  // Resolve against extensions_ (all loaded), not filtered_: a queued click
  // dispatched by processInstallQueue() must still be found even if the user
  // changed the search/category filter and it is no longer in the visible set.
  for (const auto& ext : extensions_) {
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
    // Skip extensions already staged for restart: a staged update leaves the
    // installed version unchanged (so hasUpdate() stays true) but re-queuing it
    // would just re-download and re-stage the same payload.
    if (ext_mgr_->hasUpdate(ext) && !ext_mgr_->hasPendingInstall(ext.id) && !ext_mgr_->hasPendingUninstall(ext.id)) {
      update_queue_.append(ext);
    }
  }
  if (update_queue_.isEmpty()) {
    return;
  }
  ui_->update_all_btn_->setEnabled(false);
  setStatus("Updating " + QString::number(update_queue_.size()) + " extensions...");
  populateCards();  // repaint so all queued cards show the "Installing" badge
  processInstallQueue();
}

void MarketplaceWindow::onDiagnosticsClicked() {
  Dialog dlg(this);
  dlg.setDialogTitle(tr("Marketplace Diagnostics"));
  dlg.resize(640, 360);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  auto* text = new QPlainTextEdit(body);
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
  auto* close_button = new QPushButton(tr("Close"), body);
  close_row->addWidget(close_button);
  connect(close_button, &QPushButton::clicked, &dlg, &QDialog::reject);
  layout->addLayout(close_row);
  dlg.contentLayout()->addWidget(body);
  dlg.exec();
}

void MarketplaceWindow::processInstallQueue() {
  // Wait until the current install/update finishes before dispatching the
  // next one — ExtensionManager only runs one at a time.
  if (!active_install_id_.isEmpty()) {
    return;
  }
  // Individual button clicks (pending_clicks_) run ahead of Update All
  // (update_queue_) so an explicit user click on a card is not stuck
  // behind a bulk-update batch that was already in flight.
  if (!pending_clicks_.isEmpty()) {
    const QString next_id = pending_clicks_.takeFirst();
    onActionButtonClicked(next_id);
    // If the dispatch didn't actually start an install (e.g. the extension is
    // already installed or is "local newer" by now), no installFinished will
    // fire to advance the queue — keep draining so one dead entry can't stall
    // the rest.
    if (active_install_id_.isEmpty()) {
      processInstallQueue();
    }
    return;
  }
  if (!update_queue_.isEmpty()) {
    ext_mgr_->update(update_queue_.takeFirst());
  }
}

}  // namespace PJ
