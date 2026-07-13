#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFutureSynchronizer>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <memory>

#include "pj_base/expected.hpp"

namespace PJ {

/// Handles the full extension installation pipeline: download → checksum verification → extraction.
///
/// The async fetch() operation is tracked by an integer ID returned at call time.
class DownloadManager : public QObject {
  Q_OBJECT

 public:
  /// Post-download work whose duration is not covered by byte-level `progress`.
  /// Consumers switch their UI to indeterminate/busy when a phase is announced.
  enum class WorkPhase {
    Verifying,
    Extracting,
  };
  Q_ENUM(WorkPhase)

  explicit DownloadManager(QObject* parent = nullptr);
  // Requests cancel on every in-flight worker and drains before returning.
  // Workers observe the cancel flag at each archive-entry boundary and at
  // each data block, so the wait is bounded to milliseconds rather than the
  // full extract time.
  ~DownloadManager() override;

  /// Starts the full pipeline: download url, verify expected_checksum, extract to destination_dir.
  /// Returns a unique ID to track this operation.
  int fetch(const QUrl& url, const QString& expected_checksum, const QString& destination_dir);

  /// Cancels an in-progress operation. During the checksum/extract phase this
  /// requests early exit from the worker; the consumer rolls back the
  /// transaction directory when it receives `cancelled`.
  void cancel(int id);

 signals:
  void started(int id);
  void progress(int id, qint64 bytes_received, qint64 bytes_total);
  // Reports post-download work whose duration is not covered by `progress`.
  void phaseChanged(int id, PJ::DownloadManager::WorkPhase phase);
  void finished(int id);
  void cancelled(int id);
  void failed(int id, const QString& error);

 private slots:
  void onReplyFinished(QNetworkReply* reply);
  void onDownloadProgress(qint64 bytes_received, qint64 bytes_total);

 private:
  struct Operation {
    QString expected_checksum;
    QString destination_dir;
  };

  QString calculateSha256(const QByteArray& data) const;
  bool verifyChecksum(const QByteArray& data, const QString& expected_checksum) const;
  PJ::Expected<void, QString> extractFromMemory(
      const QByteArray& data, const QString& destination_dir, const std::atomic<bool>& cancel_requested) const;

  QNetworkAccessManager* network_;
  QMap<int, QNetworkReply*> active_replies_;
  QMap<int, Operation> operations_;
  QMap<int, std::shared_ptr<std::atomic<bool>>> cancel_flags_;
  int next_id_ = 1;
  QFutureSynchronizer<QString> pending_extracts_;
};

}  // namespace PJ
