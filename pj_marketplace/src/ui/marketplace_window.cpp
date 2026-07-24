// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/marketplace_window.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_marketplace/version_compare.hpp"
#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Scrollbar.h"
#include "pj_widgets/Search.h"
#include "ui_extension_detail_dialog.h"
#include "ui_marketplace_window.h"
using namespace Qt::StringLiterals;

namespace PJ {

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
  registry_url_ = registry_url;
  // applyPendingUninstalls/applyPendingInstalls already ran in ExtensionManager::initComponents().
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  registry_url_ = registry_url;
  finishConstruction(nullptr);
}

MarketplaceWindow::MarketplaceWindow(
    ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
    QWidget* parent)
    : Dialog(parent), ui_(new Ui::MarketplaceWindow) {
  registry_mgr_ = new RegistryManager(this);
  ext_mgr_ = ext_mgr;
  initial_snapshot_provided_ = true;
  registry_url_ = registry_url;
  finishConstruction(&installed);
}

void MarketplaceWindow::finishConstruction(const QMap<QString, InstalledExtension>* installed) {
  setupUi();
  setupSignals();
  if (installed != nullptr) {
    ext_mgr_->setInstalledExtensions(*installed);
  }
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
  connect(ui_->diagnostics_btn_, &QPushButton::clicked, this, &MarketplaceWindow::onDiagnosticsClicked);

  // Master–detail split: the plugin list (left) is narrower than the detail
  // panel (right), but wide enough that a card's action button fits fully; the
  // right keeps enough room that its button row (ending in "Visit Website") is
  // never clipped by the window edge. Only this middle band is split, and the
  // user can drag the boundary (childrenCollapsible is off in the .ui so
  // neither pane can be dragged away to zero width).
  ui_->scroll_area_->setMinimumWidth(410);
  ui_->detail_scroll_->setMinimumWidth(520);
  ui_->content_split_->setStretchFactor(0, 2);
  ui_->content_split_->setStretchFactor(1, 3);

  // Hard floor on the window size so it can never be shrunk to where the two
  // panes overlap or buttons get hidden: left(360) + right(520) minimum pane
  // widths + the split spacing, dialog margins and chrome. Below this Qt simply
  // refuses to shrink further, keeping everything visible.
  setMinimumSize(1080, 580);

  // Open a touch larger than the floor so both panes and every button (card
  // action on the left, action/uninstall/website on the right) are comfortably
  // visible without the user having to resize.
  resize(1100, 640);

  // Canonical overlay pill scrollbars for the extension list / detail scroll
  // areas just built (their ranges update live as the registry loads).
  attachPillScrollbars(this);
}

// ─── Signal wiring ───────────────────────────────────────────────────────────

void MarketplaceWindow::setupSignals() {
  // RegistryManager
  connect(registry_mgr_, &RegistryManager::fetchStarted, this, [this]() { setInfoStatus("Loading registry..."); });

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
    setInfoStatus("Ready — " + QString::number(extensions_.size()) + " extensions loaded");
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

  connect(ext_mgr_, &ExtensionManager::downgradePendingRestart, this, [this](const QString& id) {
    ui_->progress_bar_->setVisible(false);
    status_error_sticky_ = false;
    populateCards();
    setStatus(QString("Extension %1 will revert to its bundled version after restart").arg(id));
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
        // Seen live while the window is open — mark it surfaced so a later
        // re-open doesn't show it again as if it were new.
        ext_mgr_->markDiagnosticsSurfaced();
        if (is_error) {
          setStatus("Marketplace diagnostic: " + message, true);
        }
      });
}

// ─── Cards Population ─────────────────────────────────────────────────────────

void MarketplaceWindow::populateCards(bool preserve_scroll) {
  // The rebuild below destroys every card, so the scroll offset is captured now
  // and reapplied after the new cards are laid out. Restoring synchronously would
  // clamp against a stale (empty) range, so it is deferred to the next event-loop
  // turn once the scroll area has recomputed its range from the rebuilt content.
  const int saved_scroll = ui_->scroll_area_->verticalScrollBar()->value();

  while (ui_->cards_layout_->count() > 1) {
    delete ui_->cards_layout_->takeAt(0)->widget();
  }

  // Keep a valid selection so the detail panel is never empty: if the current
  // selection is filtered out (or unset), fall back to the first visible card.
  const bool selection_visible =
      std::any_of(filtered_.begin(), filtered_.end(), [&](const Extension& e) { return e.id == selected_ext_id_; });
  if (!selection_visible) {
    selected_ext_id_ = filtered_.isEmpty() ? QString{} : filtered_.first().id;
  }

  const auto installed = ext_mgr_->installedExtensions();
  for (const Extension& ext : filtered_) {
    const QString ext_id = ext.id;

    auto* card = new QFrame(ui_->cards_container);
    card->setFrameShape(QFrame::NoFrame);
    card->setProperty("ext_id", ext_id);
    // Highlight the selected card (styled via QFrame#extCard[selected="true"]).
    card->setProperty("selected", ext_id == selected_ext_id_);
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
    // Two-line summary: wraps, then elides with "…" once the text exceeds two
    // lines (the full text lives in the tooltip and the detail panel).
    auto* desc_lbl = new ElidingLabel(card);
    desc_lbl->setObjectName("extCardDescription");
    desc_lbl->setMaxLineCount(2);
    desc_lbl->setFullText(ext.description);
    bottom_row->addWidget(desc_lbl, /*stretch=*/1);
    bottom_row->addLayout(btn_box);
    // Keep the button pinned to the top of a now-possibly-multiline row.
    bottom_row->setAlignment(btn_box, Qt::AlignTop);
    card_layout->addLayout(bottom_row);

    ui_->cards_layout_->insertWidget(ui_->cards_layout_->count() - 1, card);
  }

  // Enable "Update All" if ANY loaded extension has an update — not just the
  // filtered subset shown as cards — so the button's reach matches its action.
  // A staged update keeps its old installed version until restart, so
  // hasUpdate() stays true; exclude already-pending extensions so the button
  // doesn't stay enabled to re-stage what is already queued for restart.
  bool any_updatable = false;
  for (const Extension& ext : extensions_) {
    if (ext_mgr_->hasUpdate(ext) && !ext_mgr_->hasPendingInstall(ext.id) && !ext_mgr_->hasPendingUninstall(ext.id)) {
      any_updatable = true;
      break;
    }
  }
  ui_->update_all_btn_->setEnabled(any_updatable && update_queue_.isEmpty());

  // Refresh the right-hand detail panel for the (possibly updated) selection.
  showDetail(selected_ext_id_);

  if (preserve_scroll) {
    QTimer::singleShot(
        0, this, [this, saved_scroll]() { ui_->scroll_area_->verticalScrollBar()->setValue(saved_scroll); });
  }
}

// ─── Event Filter (select a card on click) ───────────────────────────────────

bool MarketplaceWindow::eventFilter(QObject* obj, QEvent* event) {
  // Select the clicked card so the right-hand detail panel shows it. The base
  // PJ::Dialog installs this object as an app-wide event filter (for resize-edge
  // cursors), so this override sees every widget's events; find the owning card
  // by walking up from the clicked widget to the first ancestor carrying an
  // "ext_id" (so a click anywhere on the card — label, empty area — selects it).
  // Never consume the event: the action button inside the card must still fire.
  if (event->type() == QEvent::MouseButtonPress) {
    for (QObject* o = obj; o != nullptr; o = o->parent()) {
      const QString ext_id = o->property("ext_id").toString();
      if (!ext_id.isEmpty()) {
        if (ext_id != selected_ext_id_) {
          selected_ext_id_ = ext_id;
          updateCardSelection();
          showDetail(ext_id);
        }
        break;
      }
    }
  }
  return Dialog::eventFilter(obj, event);
}

void MarketplaceWindow::updateCardSelection() {
  for (int i = 0; i < ui_->cards_layout_->count(); ++i) {
    QWidget* w = ui_->cards_layout_->itemAt(i)->widget();
    if (w == nullptr) {
      continue;  // the trailing vertical stretch has no widget
    }
    const QString ext_id = w->property("ext_id").toString();
    if (ext_id.isEmpty()) {
      continue;
    }
    const bool sel = ext_id == selected_ext_id_;
    if (w->property("selected").toBool() != sel) {
      w->setProperty("selected", sel);
      w->style()->unpolish(w);
      w->style()->polish(w);
    }
  }
}

void MarketplaceWindow::showDetail(const QString& ext_id) {
  // Tear down the previous panel content (widgets + layout).
  if (QLayout* old = ui_->detail_container_->layout()) {
    QLayoutItem* item = nullptr;
    while ((item = old->takeAt(0)) != nullptr) {
      delete item->widget();
      delete item;
    }
    delete old;
  }

  const Extension* ext = nullptr;
  for (const Extension& e : extensions_) {
    if (e.id == ext_id) {
      ext = &e;
      break;
    }
  }

  auto* outer = new QVBoxLayout(ui_->detail_container_);
  outer->setContentsMargins(0, 0, 0, 0);

  if (ext == nullptr) {
    outer->addStretch();  // no selection (empty list) — leave the panel blank
    return;
  }

  // Build the SAME form the modal detail dialog used, so the layout and button
  // disposition are identical — just embedded in the right panel. The Ui struct
  // is local: it only creates the widgets (owned by `body`) and gives us named
  // handles to configure them here.
  auto* body = new QWidget(ui_->detail_container_);
  Ui::ExtensionDetailDialog form;
  form.setupUi(body);
  outer->addWidget(body);
  // No "Close" button: this is an embedded panel, not a modal subdialog.
  form.close_btn->hide();

  const auto installed = ext_mgr_->installedExtensions();
  const QString installed_version = installed.contains(ext_id) ? installed[ext_id].version : QString{};
  const bool is_installed = installed.contains(ext_id);
  const bool has_update = ext_mgr_->hasUpdate(*ext);
  const bool has_newer_local = ext_mgr_->hasNewerInstalledVersion(*ext);
  const bool needs_restart = ext_mgr_->hasPendingInstall(ext_id) || ext_mgr_->hasPendingUninstall(ext_id);
  const bool in_update_queue =
      std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
  const bool installing = ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue;
  // Core (bundled) extensions ship with the app and can't be uninstalled — the
  // panel shows the Uninstall action locked (mirrors the modal detail dialog).
  const bool is_bundled = ext_mgr_->isBundled(ext_id);

  // Title.
  form.title_lbl->setText(ext->name + "  v" + ext->version);
  // Force the size via stylesheet: a QSS font-size rule from the app theme wins
  // over QFont::setPointSize(), so setFont() alone left the title unchanged.
  form.title_lbl->setStyleSheet("font-size: 20px; font-weight: 700;");

  // Metadata row.
  QStringList meta;
  if (!ext->publisher.isEmpty()) {
    meta << ext->publisher;
  }
  if (!ext->category.isEmpty()) {
    meta << ext->category;
  }
  if (!ext->license.isEmpty()) {
    meta << ext->license;
  }
  if (!ext->min_plotjuggler_version.isEmpty()) {
    meta << "requires PJ " + ext->min_plotjuggler_version + "+";
  }
  if (is_installed) {
    meta << "installed: v" + installed_version;
  }
  form.meta_lbl->setText(meta.join("  •  "));

  // Tag chips.
  for (int i = 0; i < ext->tags.size(); ++i) {
    auto* chip = new QLabel(ext->tags[i], form.tags_container);
    chip->setObjectName("extTagChip");
    form.tags_layout->insertWidget(i, chip);
  }

  // Full description, wrapped.
  form.desc_lbl->setText(ext->description);
  form.desc_lbl->setWordWrap(true);

  // GitHub / website link.
  form.github_btn->setEnabled(!ext->website.isEmpty());
  const QString website = ext->website;
  connect(form.github_btn, &QPushButton::clicked, this, [website]() {
    if (!website.isEmpty()) {
      QDesktopServices::openUrl(QUrl(website));
    }
  });

  // Exactly ONE action button, chosen by state (never all of them). action_btn
  // and uninstall_btn default hidden in the .ui, so untouched states stay off.
  if (installing) {
    form.action_btn->setText("Installing");
    form.action_btn->setObjectName("extBadgeInstalling");
    form.action_btn->setEnabled(false);
    form.action_btn->setVisible(true);
  } else if (needs_restart) {
    form.action_btn->setText("Needs Restart");
    form.action_btn->setObjectName("extBadgeNeedsRestart");
    form.action_btn->setEnabled(false);
    form.action_btn->setVisible(true);
  } else {
    if (!is_installed || has_update) {
      form.action_btn->setText(has_update ? "Update ⬆" : "Install");
      form.action_btn->setObjectName(has_update ? "extButtonUpdate" : "extButtonInstall");
      form.action_btn->setVisible(true);
      connect(form.action_btn, &QPushButton::clicked, this, [this, ext_id]() { onActionButtonClicked(ext_id); });
    } else if (has_newer_local) {
      form.action_btn->setText("Local newer");
      form.action_btn->setObjectName("extBadgeLocalNewer");
      form.action_btn->setEnabled(false);
      form.action_btn->setVisible(true);
    } else {
      form.action_btn->setText("Installed");
      form.action_btn->setObjectName("extBadgeInstalled");
      form.action_btn->setEnabled(false);
      form.action_btn->setVisible(true);
    }
    // Uninstall, shown for an installed extension (between the action and Close),
    // exactly as the old subdialog did.
    if (is_installed) {
      form.uninstall_btn->setVisible(true);
      // Empty when not core; otherwise the version the app ships, used to lock
      // uninstall (installed == bundled) or offer downgrade-to-bundled.
      const QString bundled_version = ext_mgr_->bundledVersion(ext_id);
      // For a core plugin, compare the installed version to the one it ships
      // with — via the same comparator the seed and the uninstall guard use.
      const int installed_vs_bundled =
          is_bundled ? compareSemver(installed_version.toStdString(), bundled_version.toStdString()) : 0;
      if (is_bundled && installed_vs_bundled <= 0) {
        // Core extension at its bundled version: it ships with the app and can't be
        // removed. Shown but locked, so the user sees it exists yet cannot remove it.
        form.uninstall_btn->setEnabled(false);
        form.uninstall_btn->setToolTip(tr("This extension ships with the application and cannot be uninstalled"));
      } else if (is_bundled) {
        // Core extension updated ABOVE its bundled version: offer to revert to the
        // shipped version instead of a plain uninstall. The bundled build ships with
        // the app, so it is always a compatible downgrade. Functionally this
        // uninstalls the updated copy; the seed restores the bundled version on the
        // next launch. Red style via the #extButtonDowngrade rule.
        form.uninstall_btn->setText(tr("Downgrade to bundled v%1").arg(bundled_version));
        form.uninstall_btn->setObjectName("extButtonDowngrade");
        form.uninstall_btn->setToolTip(
            tr("Reverts to the bundled version v%1 on the next launch").arg(bundled_version));
        connect(form.uninstall_btn, &QPushButton::clicked, this, [this, ext_id]() {
          clearStickyStatus();
          ext_mgr_->downgradeToBundled(ext_id);
        });
      } else {
        connect(
            form.uninstall_btn, &QPushButton::clicked, this, [this, ext_id]() { onUninstallButtonClicked(ext_id); });
      }
    }
  }

  // Same fixed width as the left-hand card buttons so the action reads identically
  // on both sides.
  form.action_btn->setFixedWidth(90);
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

  populateCards(/*preserve_scroll=*/false);
  setInfoStatus(QString::number(filtered_.size()) + " of " + QString::number(extensions_.size()) + " extensions shown");
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

void MarketplaceWindow::setInfoStatus(const QString& msg) {
  if (!active_install_id_.isEmpty()) {
    // An install owns the status line; keep it showing the live progress rather
    // than letting a filter/refresh/registry-load message desync it from the
    // still-moving progress bar.
    showInstallProgress();
    return;
  }
  setStatus(msg);
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
  // Surface a diagnostic on window open ONLY if one arrived that hasn't been
  // shown yet (e.g. a staged-promotion failure that happened at startup while
  // no window was open). Re-showing the latest unconditionally on every re-open
  // resurrected a stale error the user had already moved past.
  if (!ext_mgr_->hasUnsurfacedDiagnostics()) {
    return;
  }
  const ExtensionDiagnostic& diagnostic = ext_mgr_->diagnostics().back();
  ext_mgr_->markDiagnosticsSurfaced();
  setStatus("Marketplace diagnostic: " + diagnostic.message, diagnostic.is_error);
}

void MarketplaceWindow::updateDiagnosticsButton() {
  const int count = ext_mgr_->diagnostics().size();
  ui_->diagnostics_btn_->setVisible(count > 0);
  ui_->diagnostics_btn_->setText(count > 1 ? QString("Details (%1)").arg(count) : "Details");
}

// ─── Slots ────────────────────────────────────────────────────────────────────

void MarketplaceWindow::onSearchChanged(const QString& /*text*/) {
  // Filtering is a user action like Refresh/Install: clear a sticky error so the
  // "K of N shown" count applyFilters() emits isn't suppressed by setStatus().
  clearStickyStatus();
  applyFilters();
}
void MarketplaceWindow::onCategoryChanged(int /*index*/) {
  clearStickyStatus();
  applyFilters();
}

void MarketplaceWindow::onRefreshClicked() {
  clearStickyStatus();
  setInfoStatus("Refreshing...");
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

void MarketplaceWindow::onActionButtonClicked(const QString& ext_id) {
  // If another install/update is already in flight, queue this click and let
  // processInstallQueue() dispatch it when the current one completes.
  // Otherwise ExtensionManager::install() would reject with
  // "Install of X is already in progress" — its single-install-at-a-time
  // model is intentional, we just hide it behind a queue at the UI layer.
  if (!active_install_id_.isEmpty()) {
    const bool in_update_queue =
        std::any_of(update_queue_.begin(), update_queue_.end(), [&](const Extension& e) { return e.id == ext_id; });
    if (ext_id == active_install_id_ || pending_clicks_.contains(ext_id) || in_update_queue) {
      return;  // deduplicate — running, queued by a click, or already in the Update All batch
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
  // Iterate every loaded extension, not just the currently filtered/searched
  // subset: "Update All" means all updatable extensions, regardless of the
  // active category filter or search term. Skip extensions already staged for
  // restart: a staged update leaves the installed version unchanged (so
  // hasUpdate() stays true) but re-queuing it would just re-download and
  // re-stage the same payload.
  for (const auto& ext : extensions_) {
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
