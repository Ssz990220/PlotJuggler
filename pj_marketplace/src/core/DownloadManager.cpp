// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <archive.h>
#include <archive_entry.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkRequest>
#include <QtConcurrent>

#include "pj_base/expected.hpp"
#include "pj_marketplace/download_manager.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

PJ::Expected<void, QString> DownloadManager::extractFromMemory(
    const QByteArray& data, const QString& destination_dir, const std::atomic<bool>& cancel_requested) const {
  QDir dest_dir(destination_dir);
  if (!dest_dir.exists() && !dest_dir.mkpath(u"."_s)) {
    return PJ::unexpected(u"Could not create destination directory: %1"_s.arg(destination_dir));
  }

  // Trailing separator ensures prefix check is exact and not fooled by
  // sibling directories sharing a common prefix (e.g. /tmp/foo vs /tmp/foo_evil).
  const QString safe_root = dest_dir.absolutePath() + QLatin1Char('/');

  auto archive_deleter = [](struct archive* a) { archive_read_free(a); };
  std::unique_ptr<struct archive, decltype(archive_deleter)> a(archive_read_new(), archive_deleter);

  archive_read_support_format_zip(a.get());

  if (archive_read_open_memory(a.get(), data.constData(), static_cast<size_t>(data.size())) != ARCHIVE_OK) {
    return PJ::unexpected(u"Could not open ZIP: %1"_s.arg(QString::fromUtf8(archive_error_string(a.get()))));
  }

  struct archive_entry* entry;
  int r;
  while ((r = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    // Cancel checkpoint before starting each entry. The consumer wipes the
    // whole transaction dir on cancel, so returning here with a partial file
    // (if the previous entry was mid-write) is safe.
    if (cancel_requested.load(std::memory_order_relaxed)) {
      return PJ::unexpected(u"Cancelled"_s);
    }

    const QString entry_name = QString::fromUtf8(archive_entry_pathname(entry));
    const QString target_path = dest_dir.filePath(entry_name);

    // Guard against path-traversal attacks (e.g. entries containing "../")
    if (!QFileInfo(target_path).absoluteFilePath().startsWith(safe_root)) {
      return PJ::unexpected(u"Unsafe path detected in ZIP entry: %1"_s.arg(entry_name));
    }

    if (archive_entry_filetype(entry) == AE_IFDIR) {
      dest_dir.mkpath(entry_name);
      continue;
    }

    // Ensure the parent directory exists before writing
    QFileInfo fi(target_path);
    QDir().mkpath(fi.absolutePath());

    QFile out_file(target_path);
    if (!out_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      return PJ::unexpected(u"No write permission for: %1"_s.arg(target_path));
    }

    const void* buf;
    size_t size;
    la_int64_t offset;
    for (;;) {
      // Cancel checkpoint per block so a single huge entry cannot pin cancel
      // response time to the whole entry's write duration.
      if (cancel_requested.load(std::memory_order_relaxed)) {
        return PJ::unexpected(u"Cancelled"_s);
      }
      int rc = archive_read_data_block(a.get(), &buf, &size, &offset);
      if (rc == ARCHIVE_EOF) {
        break;
      }
      if (rc != ARCHIVE_OK) {
        out_file.close();
        return PJ::unexpected(
            u"Error reading ZIP entry '%1': %2"_s.arg(entry_name, QString::fromUtf8(archive_error_string(a.get()))));
      }
      out_file.write(static_cast<const char*>(buf), static_cast<qint64>(size));
    }
    out_file.close();
  }

  if (r != ARCHIVE_EOF) {
    return PJ::unexpected(u"Error during extraction: %1"_s.arg(QString::fromUtf8(archive_error_string(a.get()))));
  }

  return {};
}

// ---------------------------------------------------------------------------
// DownloadManager
// ---------------------------------------------------------------------------

DownloadManager::DownloadManager(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
  connect(network_, &QNetworkAccessManager::finished, this, &DownloadManager::onReplyFinished);
}

DownloadManager::~DownloadManager() {
  // Signal cancel to every in-flight worker before draining. Extract checks
  // the flag at each archive entry and each data block, so the wait is
  // bounded by one block (~ms) plus at worst one full checksum on ~100 MB
  // (~30-100 ms) rather than the whole extract time.
  for (const auto& flag : cancel_flags_) {
    flag->store(true, std::memory_order_relaxed);
  }
  pending_extracts_.waitForFinished();
}

int DownloadManager::fetch(const QUrl& url, const QString& expected_checksum, const QString& destination_dir) {
  const int id = next_id_++;

  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply* reply = network_->get(req);
  reply->setProperty("operationId", id);

  active_replies_.insert(id, reply);
  operations_.insert(id, {expected_checksum, destination_dir});

  connect(reply, &QNetworkReply::downloadProgress, this, &DownloadManager::onDownloadProgress);

  emit started(id);
  return id;
}

void DownloadManager::cancel(int id) {
  // Download phase: abort the reply, onReplyFinished will emit cancelled().
  if (QNetworkReply* reply = active_replies_.value(id, nullptr); reply) {
    reply->abort();
    return;
  }
  // Checksum/extract phase: flip the flag, the worker returns early and the
  // watcher's finished slot maps that outcome to cancelled(id).
  if (auto it = cancel_flags_.find(id); it != cancel_flags_.end()) {
    (*it)->store(true, std::memory_order_relaxed);
  }
}

void DownloadManager::onDownloadProgress(qint64 bytes_received, qint64 bytes_total) {
  auto* reply = qobject_cast<QNetworkReply*>(sender());
  if (!reply) {
    return;
  }
  emit progress(reply->property("operationId").toInt(), bytes_received, bytes_total);
}

void DownloadManager::onReplyFinished(QNetworkReply* reply) {
  const int id = reply->property("operationId").toInt();
  active_replies_.remove(id);
  const Operation op = operations_.take(id);
  if (reply->error() != QNetworkReply::NoError) {
    if (reply->error() == QNetworkReply::OperationCanceledError) {
      reply->deleteLater();
      emit cancelled(id);
      return;
    }
    const QString error = reply->errorString();
    reply->deleteLater();
    emit failed(id, error);
    return;
  }

  const QByteArray data = reply->readAll();
  reply->deleteLater();

  // Checksum verification and extraction are CPU/IO-heavy for a large artifact.
  // Run them on a worker thread so the GUI thread stays responsive; the result
  // (empty QString on success, else an error message) is delivered back here via
  // the watcher's finished() on the GUI thread, where the signals are emitted.
  //
  // Lifetime: the destructor drains pending_extracts_ before returning, so
  // `this` remains valid for the whole worker + watcher-slot lifetime.
  const QString expected = op.expected_checksum;
  const QString destination = op.destination_dir;
  auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
  cancel_flags_.insert(id, cancel_flag);

  auto* watcher = new QFutureWatcher<QString>(this);
  connect(
      watcher, &QFutureWatcher<QString>::finished, this,
      [this, id, watcher, cancel_flag]() {
        const QString error = watcher->result();
        watcher->deleteLater();
        const bool was_cancelled = cancel_flag->load(std::memory_order_relaxed);
        cancel_flags_.remove(id);
        if (was_cancelled) {
          emit cancelled(id);
          return;
        }
        if (error.isEmpty()) {
          emit finished(id);
        } else {
          emit failed(id, error);
        }
      },
      Qt::QueuedConnection);
  auto future = QtConcurrent::run([this, id, data, expected, destination, cancel_flag]() -> QString {
    // Checksum is a single-shot ~30-100 ms hash even on ~100 MB artifacts, so
    // we do not thread the cancel flag through it. If the user cancels during
    // this window, the flag is caught at the boundary check below or inside
    // extractFromMemory; the watcher slot maps the outcome to cancelled(id).
    if (!expected.isEmpty()) {
      emit phaseChanged(id, WorkPhase::Verifying);
      if (!verifyChecksum(data, expected)) {
        return u"Checksum mismatch"_s;
      }
    }
    if (cancel_flag->load(std::memory_order_relaxed)) {
      return u"Cancelled"_s;
    }
    emit phaseChanged(id, WorkPhase::Extracting);
    if (auto extract_result = extractFromMemory(data, destination, *cancel_flag); !extract_result) {
      return extract_result.error();
    }
    return {};  // success
  });
  watcher->setFuture(future);
  pending_extracts_.addFuture(future);
}

QString DownloadManager::calculateSha256(const QByteArray& data) const {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

bool DownloadManager::verifyChecksum(const QByteArray& data, const QString& expected_checksum) const {
  QString expected = expected_checksum;
  if (expected.startsWith("sha256:"_L1, Qt::CaseInsensitive)) {
    expected = expected.mid(7);
  }
  // Hex digests are case-insensitive: `toHex()` emits lowercase, but a registry
  // may list the checksum in uppercase. Compare without regard to case so a
  // correct artifact is not rejected over digit casing.
  return calculateSha256(data).compare(expected, Qt::CaseInsensitive) == 0;
}

}  // namespace PJ
