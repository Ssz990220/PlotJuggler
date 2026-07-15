#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMap>
#include <QUrl>

#include "pj_marketplace/extension.hpp"
#include "pj_marketplace/installed_extension.hpp"
#include "pj_widgets/Dialog.h"

namespace Ui {
class MarketplaceWindow;
}

namespace PJ {

class DownloadManager;
class ExtensionManager;
class RegistryManager;

// Marketplace window on the canonical app chrome (PJ::Dialog): frameless title
// bar + close, no system (window-manager) decorations.
class MarketplaceWindow : public Dialog {
  Q_OBJECT

 public:
  explicit MarketplaceWindow(const QUrl& registry_url, QWidget* parent = nullptr);

  // Uses an externally owned ExtensionManager, mainly for tests and embedding.
  explicit MarketplaceWindow(ExtensionManager* ext_mgr, const QUrl& registry_url, QWidget* parent = nullptr);

  // Uses an externally owned ExtensionManager and a caller-provided installed snapshot.
  explicit MarketplaceWindow(
      ExtensionManager* ext_mgr, const QUrl& registry_url, const QMap<QString, InstalledExtension>& installed,
      QWidget* parent = nullptr);

  ~MarketplaceWindow() override;

  // Returns true when install state changed while the dialog was open.
  bool installationsChanged() const {
    return installations_changed_;
  }

 protected:
  // Handles card hover styling and delegated button events.
  bool eventFilter(QObject* obj, QEvent* event) override;

  // Refreshes installed state before cards are painted.
  void showEvent(QShowEvent* event) override;

 private slots:
  // Updates the search filter.
  void onSearchChanged(const QString& text);

  // Updates the category filter.
  void onCategoryChanged(int index);

  // Refetches registry data and refreshes installed state.
  void onRefreshClicked();

  // Queues updates for every installed extension with a newer registry version.
  void onUpdateAllClicked();

  // Opens the registry URL settings dialog.
  void onSettingsClicked();

  // Opens a read-only view of recent marketplace diagnostics.
  void onDiagnosticsClicked();

  // Runs the primary install/update action for one extension card.
  void onActionButtonClicked(const QString& ext_id);

  // Confirms and uninstalls one installed extension.
  void onUninstallButtonClicked(const QString& ext_id);

 private:
  // Creates widgets from the .ui file and configures fixed UI affordances.
  void setupUi();

  // Connects registry, extension-manager, and widget signals.
  void setupSignals();

  // Rebuilds every extension card from filtered_. preserve_scroll keeps the
  // vertical scroll offset across the teardown/rebuild (true for install/update/
  // uninstall repaints, so the list doesn't jump to the top mid-session); pass
  // false when the card set changes meaning — filter/search/registry reload —
  // where returning to the top is the expected behaviour.
  void populateCards(bool preserve_scroll = true);

  // Applies search and category filters to the registry list.
  void applyFilters();

  // Updates the status label; error statuses remain sticky until a user action clears them.
  void setStatus(const QString& msg, bool is_error = false);

  // Allows the next non-error status update to replace an error.
  void clearStickyStatus();

  // Shows the newest diagnostic in the status bar, if one exists.
  void showLatestDiagnostic();

  // Shows or hides the diagnostics button based on diagnostic history.
  void updateDiagnosticsButton();

  // Opens the detail dialog for one registry extension.
  void openDetail(const QString& ext_id);

  // Processes one pending bulk-update item at a time.
  void processInstallQueue();

  // Shows the status of the in-flight install together with the queue depth,
  // e.g. "Installing mcap…  ·  2 queued". `verb` is the current phase word
  // ("Installing", "Verifying", "Extracting"). No-op when nothing is active, so
  // it never clobbers a terminal "Installed"/"Failed" message. Called on every
  // event that changes the active id or the queue, so the count stays live and
  // an enqueue no longer hides what is currently installing.
  void showInstallProgress(const QString& verb = QStringLiteral("Installing"));

  // "  ·  N queued" for the combined pending_clicks_ + update_queue_ depth,
  // or an empty string when nothing is waiting.
  QString queueSuffix() const;

  Ui::MarketplaceWindow* ui_ = nullptr;
  DownloadManager* download_mgr_ = nullptr;
  RegistryManager* registry_mgr_ = nullptr;
  ExtensionManager* ext_mgr_ = nullptr;
  QUrl registry_url_;

  QList<Extension> extensions_;  // populated from RegistryManager::fetchFinished
  QList<Extension> filtered_;
  QList<Extension> update_queue_;
  // Individual Install/Update button clicks that arrive while another install
  // is already running. Drained by processInstallQueue() in FIFO order once
  // active_install_id_ clears.
  QList<QString> pending_clicks_;
  // Id of the extension currently being installed or updated by the manager,
  // set from installStarted and cleared from installFinished. Empty means
  // idle — the UI-side guard uses this to decide whether to enqueue a click
  // instead of dispatching it straight to ExtensionManager::install().
  QString active_install_id_;
  bool installations_changed_ = false;
  bool status_error_sticky_ = false;
  bool initial_snapshot_provided_ = false;
};

}  // namespace PJ
