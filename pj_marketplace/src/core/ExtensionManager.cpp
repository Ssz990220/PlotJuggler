// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QUuid>
#include <QVersionNumber>
#include <filesystem>

#include "pj_marketplace/download_manager.hpp"
#include "pj_marketplace/extension_manager.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_plugins/host/plugin_catalog.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {

static constexpr const char* kPendingUninstallMarker = ".pj_pending_uninstall";
static constexpr const char* kPendingInstallIntent = ".pj_pending_install";
static constexpr const char* kQuarantinePrefix = ".pj_quarantine_";
static constexpr int kMaxDiagnostics = 50;

QString extRoot(const QString& extensions_dir, const QString& id) {
  return QDir(extensions_dir).absoluteFilePath(id);
}

QString pendingRoot(const QString& pending_dir, const QString& id) {
  return QDir(pending_dir).absoluteFilePath(id);
}

struct DirectoryDiscovery {
  bool found_plugin = false;
  QString error;
  InstalledExtension record;
};

struct PendingInstallIntent {
  bool valid = false;
  QString id;
  QString version;
  QString error;
};

QString pendingInstallIntentPath(const QString& root) {
  return QDir(root).absoluteFilePath(kPendingInstallIntent);
}

QString invalidExtensionIdReason(const QString& id) {
  if (id.isEmpty()) {
    return "Extension id is empty";
  }
  if (id == "." || id == ".." || id.contains('/') || id.contains('\\')) {
    return QString("Extension id \"%1\" is not safe for filesystem paths").arg(id);
  }
  return {};
}

QString makeTransactionRoot(const QString& parent, const QString& id) {
  return QDir(parent).absoluteFilePath(
      QString(".pj_install_%1_%2").arg(id, QUuid::createUuid().toString(QUuid::Id128)));
}

QString candidateRoot(const QString& transaction_root, const QString& id) {
  return QDir(transaction_root).absoluteFilePath(id);
}

void removeDirectoryIfSet(const QString& path) {
  if (!path.isEmpty()) {
    QDir(path).removeRecursively();
  }
}

bool isTransactionDirectoryName(const QString& name) {
  return name.startsWith(".pj_install_");
}

QString validateTransactionContents(const QString& transaction_root, const QString& expected_id) {
  const QDir tx_dir(transaction_root);
  const QFileInfoList entries =
      tx_dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
  if (entries.size() != 1 || !entries.first().isDir() || entries.first().fileName() != expected_id) {
    return QString("Downloaded artifact must contain exactly one top-level directory named \"%1\"").arg(expected_id);
  }
  return {};
}

DirectoryDiscovery discoverExtensionDirectory(const QString& ext_root) {
  DirectoryDiscovery result;
  const auto scan = scanPluginDsos(std::filesystem::path(ext_root.toStdString()));
  if (!scan) {
    result.error = QString::fromStdString(scan.error());
    return result;
  }

  for (const auto& diag : scan->diagnostics) {
    qWarning(
        "ExtensionManager: plugin discovery diagnostic for '%s': %s", diag.path.string().c_str(), diag.message.c_str());
  }

  if (scan->plugins.empty()) {
    if (!scan->diagnostics.empty()) {
      result.error = QString::fromStdString(scan->diagnostics.front().message);
      return result;
    }
    result.error = u"no valid plugin DSO found"_s;
    return result;
  }

  const PluginDescriptor& first = scan->plugins.front();
  for (const PluginDescriptor& descriptor : scan->plugins) {
    if (descriptor.id != first.id) {
      result.error = u"multiple embedded plugin ids in one extension directory: \"%1\" and \"%2\""_s.arg(
          QString::fromStdString(first.id), QString::fromStdString(descriptor.id));
      return result;
    }
    if (descriptor.version != first.version) {
      result.error = u"multiple embedded plugin versions in one extension directory for \"%1\""_s.arg(
          QString::fromStdString(first.id));
      return result;
    }
  }

  result.found_plugin = true;
  result.record.id = QString::fromStdString(first.id);
  result.record.version = QString::fromStdString(first.version);
  result.record.install_date = QFileInfo(ext_root).lastModified();
  result.record.path = ext_root;
  result.record.enabled = true;
  return result;
}

QString validateRegistryIntent(
    const DirectoryDiscovery& discovered, const QString& registry_id, const QString& registry_version) {
  if (!discovered.found_plugin) {
    return QString("Installed artifact is not a valid plugin: %1").arg(discovered.error);
  }
  if (discovered.record.id != registry_id) {
    return QString("Embedded plugin id \"%1\" does not match registry id \"%2\"")
        .arg(discovered.record.id, registry_id);
  }
  if (discovered.record.version != registry_version) {
    return QString("Embedded plugin version \"%1\" does not match registry version \"%2\"")
        .arg(discovered.record.version, registry_version);
  }
  return {};
}

bool writePendingInstallIntent(const QString& root, const Extension& ext, QString* error) {
  QFile file(pendingInstallIntentPath(root));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error != nullptr) {
      *error = QString("Could not write staged install intent: %1").arg(file.errorString());
    }
    return false;
  }

  const QByteArray data = ext.id.toUtf8() + '\n' + ext.version.toUtf8() + '\n';
  if (file.write(data) != data.size()) {
    if (error != nullptr) {
      *error = QString("Could not write staged install intent: %1").arg(file.errorString());
    }
    return false;
  }
  return true;
}

PendingInstallIntent readPendingInstallIntent(const QString& root) {
  PendingInstallIntent intent;
  QFile file(pendingInstallIntentPath(root));
  if (!file.exists()) {
    intent.error = "Staged install is missing registry intent";
    return intent;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    intent.error = QString("Could not read staged install intent: %1").arg(file.errorString());
    return intent;
  }

  const QList<QByteArray> lines = file.readAll().split('\n');
  if (lines.size() < 2 || lines[0].trimmed().isEmpty() || lines[1].trimmed().isEmpty()) {
    intent.error = "Staged install registry intent is invalid";
    return intent;
  }

  const QString id = QString::fromUtf8(lines[0].trimmed());
  const QString version = QString::fromUtf8(lines[1].trimmed());
  // Defend against tampered or corrupted intent files: a path-traversal id, or a
  // version that contains anything outside the semver alphabet, must not be
  // trusted later as a directory name or version comparison input.
  if (const QString id_error = invalidExtensionIdReason(id); !id_error.isEmpty()) {
    intent.error = QString("Staged install registry intent has unsafe id: %1").arg(id_error);
    return intent;
  }
  static const QRegularExpression k_version_re(u"^[0-9A-Za-z._+-]+$"_s);
  if (!k_version_re.match(version).hasMatch()) {
    intent.error = QString("Staged install registry intent has unsafe version \"%1\"").arg(version);
    return intent;
  }

  intent.valid = true;
  intent.id = id;
  intent.version = version;
  return intent;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ExtensionManager::ExtensionManager()
    : QObject(nullptr), extensions_dir_(PlatformUtils::extensionsDir()), pending_dir_(PlatformUtils::pendingDir()) {
  initComponents();
}

ExtensionManager::ExtensionManager(
    DownloadManager* downloader, const QString& extensions_dir, const QString& pending_dir, DiagnosticSink sink,
    QObject* parent)
    : QObject(parent),
      downloader_(downloader),
      extensions_dir_(extensions_dir),
      pending_dir_(pending_dir),
      sink_(std::move(sink)) {
  initComponents();
}

void ExtensionManager::initComponents() {
  if (!downloader_) {
    downloader_ = new DownloadManager(this);
  }
  if (!QDir().mkpath(extensions_dir_)) {
    reportDiagnostic({}, QString("Could not create extensions directory \"%1\"").arg(extensions_dir_), true);
  }
  // Drain restart-deferred work, THEN snapshot installed state. The order is
  // load-bearing because of a glibc dlopen quirk: once an extension's .so has
  // been opened in this process, dlopen keeps returning that first-loaded image
  // for the same path name (plugin DSOs carry STB_GNU_UNIQUE symbols, so dlclose
  // never unloads them). So an extension path must NOT be opened before a pending
  // update replaces it — otherwise every later scan (including the marketplace
  // window's showEvent refresh) reads the stale old version until the next
  // restart. applyPendingInstalls promotes WITHOUT opening the old directory (it
  // names the backup without inspecting it), and refreshInstalledFromDisk runs
  // AFTER promotion, so the path's first in-process open is the new version.
  applyPendingUninstalls();
  applyPendingInstalls();
  refreshInstalledFromDisk();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void ExtensionManager::install(const Extension& ext) {
  refreshInstalledFromDisk();
  doInstall(ext, /*staging=*/false);
}

void ExtensionManager::doInstall(const Extension& ext, bool staging, bool allow_existing) {
  if (const QString id_error = invalidExtensionIdReason(ext.id); !id_error.isEmpty()) {
    emitInstallFailure(ext.id, id_error);
    return;
  }

  if (!pending_id_.isEmpty()) {
    emitInstallFailure(ext.id, QString("Install of \"%1\" is already in progress").arg(pending_id_));
    return;
  }

  if (!allow_existing && isInstalled(ext.id)) {
    emitInstallFailure(ext.id, QString("Extension \"%1\" is already installed").arg(ext.id));
    return;
  }

  const QString platform = PlatformUtils::currentPlatform();
  if (!ext.platforms.contains(platform)) {
    emitInstallFailure(ext.id, QString("No artifact available for platform \"%1\"").arg(platform));
    return;
  }
  const Platform& artifact = ext.platforms[platform];

  // Extraction goes into a hidden transaction directory on the same filesystem
  // as the final destination, so the eventual rename is atomic. For a deferred
  // update that's pending_dir_ (promoted at next startup); for an immediate
  // fresh install we extract beside extensions_dir_ and rename in-place after
  // validation.
  // The DSO is dlopened and its embedded manifest verified inside the
  // transaction directory BEFORE the rename, then re-verified at the final
  // location AFTER the rename — see the post-promotion check below.
  const QString dest_dir = staging ? pending_dir_ : extensions_dir_;
  QDir().mkpath(dest_dir);
  const QString transaction_root = makeTransactionRoot(dest_dir, ext.id);
  // Create the transaction root eagerly so the refresh guard has a real path
  // to protect from the moment install() returns — otherwise a Refresh that
  // fires during the download window would find no directory to skip and the
  // guard would appear untested even though it is exercised in practice.
  QDir().mkpath(transaction_root);

  pending_id_ = ext.id;
  pending_extract_dir_ = QDir::cleanPath(transaction_root);
  emit installStarted(ext.id);

  dl_progress_conn_ =
      connect(downloader_, &DownloadManager::progress, this, [this](int id, qint64 received, qint64 total) {
        if (id != pending_op_id_) {
          return;
        }

        if (total > 0 && !disk_space_checked_) {
          disk_space_checked_ = true;
          constexpr qint64 kExtractionOverheadFactor = 3;
          if (QStorageInfo(extensions_dir_).bytesAvailable() < total * kExtractionOverheadFactor) {
            cancel_reason_ = "Not enough disk space to install the extension";
            downloader_->cancel(pending_op_id_);
            return;
          }
        }

        const int percent = (total > 0) ? static_cast<int>(received * 100 / total) : 0;
        emit installProgress(pending_id_, percent);
      });

  dl_phase_conn_ =
      connect(downloader_, &DownloadManager::phaseChanged, this, [this](int id, DownloadManager::WorkPhase phase) {
        if (id != pending_op_id_) {
          return;
        }
        emit installPhase(pending_id_, phase);
      });

  dl_finished_conn_ =
      connect(downloader_, &DownloadManager::finished, this, [this, ext, staging, transaction_root](int id) {
        if (id != pending_op_id_) {
          return;
        }
        disconnectDlConns();
        disk_space_checked_ = false;

        const QString finished_id = pending_id_;
        pending_id_.clear();
        pending_op_id_ = -1;

        auto fail_after_extraction = [&](const QString& message) {
          removeDirectoryIfSet(transaction_root);
          pending_extract_dir_.clear();
          emitInstallFailure(finished_id, message);
        };

        if (const QString tx_error = validateTransactionContents(transaction_root, ext.id); !tx_error.isEmpty()) {
          fail_after_extraction(tx_error);
          return;
        }

        const QString root = candidateRoot(transaction_root, ext.id);
        const DirectoryDiscovery discovered = discoverExtensionDirectory(root);
        const QString validation_error = validateRegistryIntent(discovered, ext.id, ext.version);
        if (!validation_error.isEmpty()) {
          fail_after_extraction(validation_error);
          return;
        }

        if (staging) {
          QString intent_error;
          if (!writePendingInstallIntent(root, ext, &intent_error)) {
            fail_after_extraction(intent_error);
            return;
          }

          const QString staged_root = pendingRoot(pending_dir_, ext.id);
          if (QDir(staged_root).exists() && !QDir(staged_root).removeRecursively()) {
            fail_after_extraction(
                QString("Could not replace existing staged install directory \"%1\"").arg(staged_root));
            return;
          }
          if (!QDir().rename(root, staged_root)) {
            fail_after_extraction(QString("Could not stage install to \"%1\"").arg(staged_root));
            return;
          }

          removeDirectoryIfSet(transaction_root);
          pending_extract_dir_.clear();
          pending_backup_path_.clear();
          emit installPendingRestart(finished_id);
          return;
        }

        const QString dst = extRoot(extensions_dir_, ext.id);
        // Replace any prior copy of this id stored under a different directory
        // name so we don't leave a duplicate alongside the promoted "<id>" dir.
        replaceConflictingInstallDirs(ext.id, dst);
        if (QDir(dst).exists() && !QDir(dst).removeRecursively()) {
          fail_after_extraction(QString("Could not replace existing extension directory \"%1\"").arg(dst));
          return;
        }
        if (!QDir().rename(root, dst)) {
          fail_after_extraction(QString("Could not promote install to \"%1\"").arg(dst));
          return;
        }

        // Double-check: the DSO loaded from the staging area; confirm it still
        // loads from its final location. Catches issues like rpath/relative-path
        // assumptions that hold in pending_dir_ but break in extensions_dir_.
        const DirectoryDiscovery final_check = discoverExtensionDirectory(dst);
        const QString final_error = validateRegistryIntent(final_check, ext.id, ext.version);
        if (!final_error.isEmpty()) {
          QDir(dst).removeRecursively();
          fail_after_extraction(QString("Post-promotion validation failed: %1").arg(final_error));
          return;
        }

        removeDirectoryIfSet(transaction_root);
        pending_extract_dir_.clear();
        pending_backup_path_.clear();
        registerInstalledExtension(ext.id, dst, final_check.record);
        emit installFinished(finished_id, true);
      });

  dl_failed_conn_ =
      connect(downloader_, &DownloadManager::failed, this, [this, transaction_root](int id, const QString& error) {
        if (id != pending_op_id_) {
          return;
        }
        disconnectDlConns();
        disk_space_checked_ = false;

        const QString failed_id = pending_id_;
        pending_id_.clear();
        pending_op_id_ = -1;
        pending_extract_dir_.clear();

        removeDirectoryIfSet(transaction_root);
        emitInstallFailure(failed_id, error);
      });

  dl_cancelled_conn_ = connect(downloader_, &DownloadManager::cancelled, this, [this, transaction_root](int id) {
    if (id != pending_op_id_) {
      return;
    }
    disconnectDlConns();

    const QString cancelled_id = pending_id_;
    pending_id_.clear();
    pending_op_id_ = -1;
    disk_space_checked_ = false;
    pending_extract_dir_.clear();

    removeDirectoryIfSet(transaction_root);

    const QString reason = cancel_reason_.isEmpty() ? "Installation was cancelled" : cancel_reason_;
    cancel_reason_.clear();
    emitInstallFailure(cancelled_id, reason);
  });

  pending_op_id_ = downloader_->fetch(QUrl(artifact.url), artifact.checksum, transaction_root);
}

void ExtensionManager::uninstall(const QString& extension_id) {
  refreshInstalledFromDisk();

  if (!installed_.contains(extension_id)) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is not installed").arg(extension_id));
    return;
  }

  // A core extension AT its bundled version cannot be removed — it ships with the
  // application. One updated ABOVE its bundled version can be reverted (the UI's
  // "downgrade to bundled"): allow the uninstall here, and the seed restores the
  // bundled version on the next launch. This is the backend guard mirroring the UI.
  if (isBundled(extension_id)) {
    const QVersionNumber installed_ver = QVersionNumber::fromString(installed_[extension_id].version);
    const QVersionNumber bundled_ver = QVersionNumber::fromString(bundledVersion(extension_id));
    if (QVersionNumber::compare(installed_ver, bundled_ver) <= 0) {
      emitUninstallFailure(
          extension_id,
          QString("Extension \"%1\" ships with the application and cannot be uninstalled").arg(extension_id));
      return;
    }
  }

  const QString dir_path = installed_[extension_id].path;

  if (!QDir(dir_path).removeRecursively()) {
    if (PlatformUtils::isWindows()) {
      if (!schedulePendingUninstall(dir_path)) {
        emitUninstallFailure(
            extension_id, QString("Could not mark \"%1\" for restart cleanup; uninstall not scheduled").arg(dir_path));
        return;
      }
      installed_.remove(extension_id);
      emit uninstallPendingRestart(extension_id);
    } else {
      emitUninstallFailure(
          extension_id, QString("Could not remove directory \"%1\" — the plugin may still be loaded").arg(dir_path));
    }
    return;
  }

  installed_.remove(extension_id);
  emit uninstallFinished(extension_id, true);
}

void ExtensionManager::downgradeToBundled(const QString& extension_id) {
  refreshInstalledFromDisk();

  if (!installed_.contains(extension_id)) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is not installed").arg(extension_id));
    return;
  }
  if (!isBundled(extension_id)) {
    emitUninstallFailure(
        extension_id, QString("Extension \"%1\" does not ship with the application").arg(extension_id));
    return;
  }
  const QVersionNumber installed_ver = QVersionNumber::fromString(installed_[extension_id].version);
  const QVersionNumber bundled_ver = QVersionNumber::fromString(bundledVersion(extension_id));
  if (QVersionNumber::compare(installed_ver, bundled_ver) <= 0) {
    emitUninstallFailure(extension_id, QString("Extension \"%1\" is already at its bundled version").arg(extension_id));
    return;
  }

  // Stage the removal of the updated copy — do NOT delete now: an immediate remove
  // would hot-swap the loaded DSO and the card would read "Install". Mark it for
  // restart cleanup so the card shows "Needs Restart"; on the next launch
  // applyPendingUninstalls removes it and the host seed restores the bundled
  // version (always compatible, since it ships with the app).
  const QString dir_path = installed_[extension_id].path;
  if (!schedulePendingUninstall(dir_path)) {
    emitUninstallFailure(extension_id, QString("Could not stage the downgrade of \"%1\"").arg(extension_id));
    return;
  }
  installed_.remove(extension_id);
  emit downgradePendingRestart(extension_id);
}

void ExtensionManager::update(const Extension& ext) {
  refreshInstalledFromDisk();

  // Updates defer to restart: the new version is staged in pending_dir_ and
  // promoted — backing up the old version first — by applyPendingInstalls() at
  // the next startup. This avoids hot-swapping a DSO the running session may
  // still have loaded; the installed version stays live until the user restarts.
  // (Fresh install() still promotes immediately, since there is no loaded
  // version to clash with.)
  if (hasNewerInstalledVersion(ext)) {
    emitInstallFailure(
        ext.id, QString("Installed version \"%1\" is newer than registry version \"%2\"; downgrade is not allowed")
                    .arg(installed_[ext.id].version, ext.version));
    return;
  }

  // An update already staged for this id — install OR uninstall — would just
  // be re-downloaded and re-staged on top of the pending marker; the installed
  // version does not change until restart, so a caller relying on hasUpdate()
  // can ask again. Refuse rather than redo the work. Both markers matter: on
  // Windows the uninstall path stages the removal, so a subsequent update()
  // during that window would resurrect an id the user just asked to remove.
  if (hasPendingInstall(ext.id) || hasPendingUninstall(ext.id)) {
    emitInstallFailure(ext.id, QString("Update for \"%1\" is already staged; restart to apply it").arg(ext.id));
    return;
  }

  doInstall(ext, /*staging=*/true, /*allow_existing=*/true);
}

void ExtensionManager::replaceConflictingInstallDirs(const QString& id, const QString& keep_dir) {
  const QString keep = QDir::cleanPath(keep_dir);
  const QString pending_clean = QDir::cleanPath(pending_dir_);
  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString root = entry.absoluteFilePath();
    const QString clean = QDir::cleanPath(root);
    // Never touch the promotion target itself, the staging area, or transaction/
    // quarantine scratch dirs.
    if (clean == keep || clean == pending_clean) {
      continue;
    }
    const QString name = entry.fileName();
    if (isTransactionDirectoryName(name) || name.startsWith(kQuarantinePrefix)) {
      continue;
    }
    const DirectoryDiscovery item = discoverExtensionDirectory(root);
    if (!item.found_plugin || item.record.id != id) {
      continue;  // ids are unique per extension, so only a true prior copy matches
    }
    // Move the shadow copy into the backup area (kept for recovery); fall back to
    // outright removal if the rename fails. Either way it must leave extensions_dir_
    // so the promoted "<id>" directory becomes the sole install of this extension.
    QDir().mkpath(PlatformUtils::backupDir());
    const QString backup = QDir(PlatformUtils::backupDir())
                               .absoluteFilePath(id + "-replaced-" + QUuid::createUuid().toString(QUuid::Id128));
    if (QDir().rename(root, backup)) {
      qWarning(
          "ExtensionManager: replaced prior install of '%s' at '%s' (backed up to '%s')", qPrintable(id),
          qPrintable(root), qPrintable(backup));
    } else if (QDir(root).removeRecursively()) {
      qWarning("ExtensionManager: removed prior install of '%s' at '%s'", qPrintable(id), qPrintable(root));
    } else {
      qWarning("ExtensionManager: could not remove prior install of '%s' at '%s'", qPrintable(id), qPrintable(root));
    }
    installed_.remove(id);
  }
}

void ExtensionManager::applyPendingInstalls() {
  const QDir pending(pending_dir_);
  if (!pending.exists()) {
    return;
  }

  for (const QFileInfo& entry :
       pending.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString staged_dir = entry.absoluteFilePath();
    const QString staged_name = entry.fileName();
    if (isTransactionDirectoryName(staged_name)) {
      removeDirectoryIfSet(staged_dir);
      continue;
    }
    if (staged_name.startsWith(kQuarantinePrefix)) {
      // Leftover from a previous failed promotion. Skip — manual inspection only.
      continue;
    }

    auto fail_staged_install = [&](const QString& signal_id, const QString& message) {
      qWarning(
          "ExtensionManager: staged install '%s' failed validation: %s", qPrintable(staged_dir), qPrintable(message));
      QString final_message = message;
      if (!QDir(staged_dir).removeRecursively()) {
        // Removal can fail on Windows when the DSO is still locked by another
        // process. Move the broken stage aside so the next startup does not
        // re-validate the same payload and emit the same diagnostic forever.
        const QString quarantine =
            QDir(pending_dir_)
                .absoluteFilePath(
                    QString(kQuarantinePrefix) + entry.fileName() + "_" + QUuid::createUuid().toString(QUuid::Id128));
        if (QDir().rename(staged_dir, quarantine)) {
          final_message += QString(" Moved to quarantine: \"%1\".").arg(quarantine);
        } else {
          final_message += QString(" Could not remove or quarantine \"%1\" — manual cleanup required.").arg(staged_dir);
        }
      }
      emitInstallFailure(signal_id, final_message);
    };

    if (staged_name.isEmpty()) {
      fail_staged_install(staged_name, "Staged install directory has no id");
      continue;
    }

    const PendingInstallIntent intent = readPendingInstallIntent(staged_dir);
    if (!intent.valid) {
      fail_staged_install(staged_name, intent.error);
      continue;
    }
    if (intent.id != staged_name) {
      fail_staged_install(
          staged_name,
          QString("Staged install directory \"%1\" does not match registry id \"%2\"").arg(staged_name, intent.id));
      continue;
    }

    const DirectoryDiscovery discovered = discoverExtensionDirectory(staged_dir);
    const QString validation_error = validateRegistryIntent(discovered, intent.id, intent.version);
    if (!validation_error.isEmpty()) {
      fail_staged_install(intent.id, validation_error);
      continue;
    }

    const QString dst = extRoot(extensions_dir_, intent.id);

    // Replace any prior copy of this id stored under a different directory name
    // (e.g. a bundled plugin) so promoting to "<id>" does not leave a duplicate.
    replaceConflictingInstallDirs(intent.id, dst);

    // Back up the existing install before the staged version takes its place:
    // move the current dir aside first, so promoting an update never silently
    // destroys the previous install (this is the single backup point for updates).
    pending_backup_path_.clear();
    if (QDir(dst).exists()) {
      // Name the backup WITHOUT opening the old directory. dlopening dst here to
      // read its version would pin its old image in the process (dlopen caches by
      // path name and plugin DSOs are effectively NODELETE via unique symbols),
      // so the post-promotion re-scan of dst would keep reading the old version.
      // A UUID keeps the backup unique; the embedded manifest inside still records
      // the version for anyone inspecting the backup.
      QDir().mkpath(PlatformUtils::backupDir());
      const QString candidate = QDir(PlatformUtils::backupDir())
                                    .absoluteFilePath(intent.id + "-" + QUuid::createUuid().toString(QUuid::Id128));

      if (!QDir().rename(dst, candidate)) {
        qWarning("ExtensionManager: failed to back up '%s' before promoting staged install", qPrintable(dst));
        emitInstallFailure(
            intent.id,
            QString("Could not back up \"%1\" before update — staged install left in \"%2\"").arg(dst, staged_dir));
        continue;
      }
      pending_backup_path_ = candidate;
      installed_.remove(intent.id);
    }

    if (!QDir().rename(staged_dir, dst)) {
      qWarning(
          "ExtensionManager: failed to promote staged install '%s' to '%s'", qPrintable(staged_dir), qPrintable(dst));
      QString message = QString("Could not promote staged install to \"%1\"").arg(dst);

      // Best-effort rollback so the user is never left with no extension. If
      // the rollback rename also fails, leave pending_backup_path_ set so
      // emitInstallFailure appends "Previous version remains in backup". The
      // restored directory is re-registered by the refreshInstalledFromDisk()
      // that runs after this drain (the path was never opened before, so that
      // scan reads it fresh).
      if (!pending_backup_path_.isEmpty() && QDir().rename(pending_backup_path_, dst)) {
        message += " Previous version restored.";
        pending_backup_path_.clear();
      }
      emitInstallFailure(intent.id, message);
      continue;
    }

    QFile::remove(pendingInstallIntentPath(dst));
    registerInstalledExtension(intent.id, dst, discovered.record);
    pending_backup_path_.clear();
    emit installFinished(intent.id, true);
  }
}

void ExtensionManager::applyPendingUninstalls() {
  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    if (!QFile::exists(entry.absoluteFilePath() + "/" + kPendingUninstallMarker)) {
      continue;
    }
    const QString id = entry.fileName();
    if (QDir(entry.absoluteFilePath()).removeRecursively()) {
      if (!id.isEmpty()) {
        installed_.remove(id);
      }
    } else {
      // Leave the marker in place so the next startup retries; surface so the
      // user sees that a deferred uninstall is stuck.
      reportDiagnostic(
          id,
          QString("Could not remove extension directory \"%1\"; restart the application or close any process using it")
              .arg(entry.absoluteFilePath()),
          true);
    }
  }
}

bool ExtensionManager::isInstalled(const QString& id) const {
  return installed_.contains(id);
}

void ExtensionManager::setBundledVersions(const QMap<QString, QString>& id_to_version) {
  bundled_versions_ = id_to_version;
}

bool ExtensionManager::isBundled(const QString& id) const {
  // "Core" = the id ships with the application. The host computes the bundled
  // id -> version map from the bundled plugin directory and hands it in via
  // setBundledVersions(); no per-folder marker. Empty (standalone marketplace app
  // or a --plugin-dir run) means nothing is core.
  return bundled_versions_.contains(id);
}

QString ExtensionManager::bundledVersion(const QString& id) const {
  return bundled_versions_.value(id);
}

bool ExtensionManager::hasPendingInstall(const QString& id) const {
  if (!invalidExtensionIdReason(id).isEmpty()) {
    return false;
  }
  const QString root = pendingRoot(pending_dir_, id);
  const PendingInstallIntent intent = readPendingInstallIntent(root);
  if (!intent.valid || intent.id != id) {
    return false;
  }
  const DirectoryDiscovery discovered = discoverExtensionDirectory(root);
  return validateRegistryIntent(discovered, intent.id, intent.version).isEmpty();
}

bool ExtensionManager::hasPendingUninstall(const QString& id) const {
  return QFile::exists(extRoot(extensions_dir_, id) + "/" + kPendingUninstallMarker);
}

bool ExtensionManager::hasNewerInstalledVersion(const Extension& ext) const {
  if (!installed_.contains(ext.id)) {
    return false;
  }

  const QVersionNumber installed_ver = QVersionNumber::fromString(installed_[ext.id].version);
  const QVersionNumber registry_ver = QVersionNumber::fromString(ext.version);
  return QVersionNumber::compare(installed_ver, registry_ver) > 0;
}

bool ExtensionManager::hasUpdate(const Extension& ext) const {
  if (!installed_.contains(ext.id)) {
    return false;
  }

  const QVersionNumber installed_ver = QVersionNumber::fromString(installed_[ext.id].version);
  const QVersionNumber latest = QVersionNumber::fromString(ext.version);
  return QVersionNumber::compare(latest, installed_ver) > 0;
}

QMap<QString, InstalledExtension> ExtensionManager::installedExtensions() const {
  return installed_;
}

QList<ExtensionDiagnostic> ExtensionManager::diagnostics() const {
  return diagnostics_;
}

void ExtensionManager::clearDiagnostics() {
  diagnostics_.clear();
  diagnostics_surfaced_ = diagnostics_recorded_;
}

bool ExtensionManager::hasUnsurfacedDiagnostics() const {
  return !diagnostics_.isEmpty() && diagnostics_recorded_ > diagnostics_surfaced_;
}

void ExtensionManager::markDiagnosticsSurfaced() {
  diagnostics_surfaced_ = diagnostics_recorded_;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void ExtensionManager::disconnectDlConns() {
  disconnect(dl_progress_conn_);
  disconnect(dl_phase_conn_);
  disconnect(dl_finished_conn_);
  disconnect(dl_failed_conn_);
  disconnect(dl_cancelled_conn_);
}

bool ExtensionManager::schedulePendingUninstall(const QString& path) {
  QFile marker(path + "/" + kPendingUninstallMarker);
  // Content is irrelevant; existence is the signal. Failure here means the next
  // startup's applyPendingUninstalls() will not see the marker and the directory
  // would leak forever — surface it so the caller can fail the uninstall.
  return marker.open(QIODevice::WriteOnly);
}

void ExtensionManager::reportDiagnostic(const QString& id, const QString& message, bool is_error) {
  diagnostics_.append(ExtensionDiagnostic{id, message, is_error, QDateTime::currentDateTimeUtc()});
  ++diagnostics_recorded_;
  while (diagnostics_.size() > kMaxDiagnostics) {
    diagnostics_.removeFirst();
  }
  emit diagnosticReported(id, message, is_error);
  if (sink_) {
    sink_(
        Diagnostic{
            is_error ? DiagnosticLevel::kError : DiagnosticLevel::kInfo,
            "ExtensionManager",
            id.toStdString(),
            message.toStdString(),
            std::chrono::system_clock::now(),
        });
  }
}

void ExtensionManager::emitInstallFailure(const QString& id, const QString& message) {
  QString diagnostic = message;
  if (!pending_backup_path_.isEmpty()) {
    diagnostic += QString(" Previous version remains in backup: \"%1\".").arg(pending_backup_path_);
  }
  pending_backup_path_.clear();
  reportDiagnostic(id, diagnostic, true);
  emit installError(id, diagnostic);
  emit installFinished(id, false);
}

void ExtensionManager::emitUninstallFailure(const QString& id, const QString& message) {
  reportDiagnostic(id, message, true);
  emit uninstallError(id, message);
  emit uninstallFinished(id, false);
}

void ExtensionManager::registerInstalledExtension(const QString& id, const QString& dst, InstalledExtension record) {
  record.path = dst;
  record.install_date = QFileInfo(dst).lastModified();
  installed_[id] = record;
}

void ExtensionManager::refreshInstalledFromDisk() {
  QMap<QString, InstalledExtension> discovered;
  const QDir dir(extensions_dir_);
  for (const QFileInfo& entry : dir.entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    const QString root = entry.absoluteFilePath();
    if (isTransactionDirectoryName(entry.fileName())) {
      // Skip the transaction dir of an install that is still in progress —
      // the worker is writing into it in the background, and its completion
      // handler owns cleanup. Wiping it here (triggered by Refresh, or by
      // starting an install/uninstall/update of a different plugin) would
      // race with the worker and truncate a partial install into a broken
      // final state. Both sides are normalized so the guard tolerates
      // trailing slashes, `..` segments, and case-insensitive filesystems.
      if (QDir::cleanPath(root) != pending_extract_dir_) {
        removeDirectoryIfSet(root);
      }
      continue;
    }
    if (QFile::exists(root + "/" + kPendingUninstallMarker)) {
      continue;
    }

    const DirectoryDiscovery item = discoverExtensionDirectory(root);
    if (!item.found_plugin) {
      qWarning("ExtensionManager: ignoring extension directory '%s': %s", qPrintable(root), qPrintable(item.error));
      continue;
    }
    if (discovered.contains(item.record.id)) {
      // Two directories embed the same id (e.g. a promoted update left alongside
      // a differently-named prior copy). Keep the HIGHEST version rather than
      // whichever the directory scan happened to hit first, so the resolution is
      // deterministic and an update never loses to a stale lower-version copy.
      const QVersionNumber existing = QVersionNumber::fromString(discovered[item.record.id].version);
      const QVersionNumber candidate = QVersionNumber::fromString(item.record.version);
      if (QVersionNumber::compare(candidate, existing) <= 0) {
        qWarning(
            "ExtensionManager: duplicate embedded id '%s' in '%s'; keeping higher version already found",
            qPrintable(item.record.id), qPrintable(root));
        continue;
      }
      qWarning(
          "ExtensionManager: duplicate embedded id '%s'; '%s' supersedes the lower-version copy",
          qPrintable(item.record.id), qPrintable(root));
    }
    discovered[item.record.id] = item.record;
  }
  installed_ = std::move(discovered);
}

void ExtensionManager::setInstalledExtensions(QMap<QString, InstalledExtension> installed) {
  installed_ = std::move(installed);
}

}  // namespace PJ
