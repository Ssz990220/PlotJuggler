// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <archive.h>
#include <archive_entry.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>

#include "pj_base/expected.hpp"
#include "pj_marketplace/download_manager.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

PJ::Expected<void, QString> DownloadManager::extractFromMemory(
    const QByteArray& data, const QString& destination_dir) const {
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
  QNetworkReply* reply = active_replies_.value(id, nullptr);
  if (reply) {
    reply->abort();
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

  if (!op.expected_checksum.isEmpty() && !verifyChecksum(data, op.expected_checksum)) {
    emit failed(id, u"Checksum mismatch"_s);
    return;
  }

  auto extract_result = extractFromMemory(data, op.destination_dir);
  if (!extract_result) {
    emit failed(id, extract_result.error());
    return;
  }

  emit finished(id);
}

QString DownloadManager::calculateSha256(const QByteArray& data) const {
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

bool DownloadManager::verifyChecksum(const QByteArray& data, const QString& expected_checksum) const {
  QString expected = expected_checksum;
  if (expected.startsWith("sha256:"_L1)) {
    expected = expected.mid(7);
  }
  return calculateSha256(data) == expected;
}

}  // namespace PJ
