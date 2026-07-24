#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace PJ {

// A newer release as reported by GitHub. `name` is the human title (the tag
// name when GitHub's `name` field is empty); `html_url` is the release page.
struct ReleaseInfo {
  QString name;
  QString html_url;
};

// One-shot "is there a newer release on GitHub?" check.
//
// checkLatestRelease() GETs the repo's `releases/latest` endpoint and, comparing
// the returned tag against the running version (QCoreApplication::applicationVersion()
// by default), emits AT MOST ONE of: updateAvailable, upToDate, or checkFailed
// (a check superseded by a newer checkLatestRelease() is aborted and emits
// nothing). Every failure mode — network error, HTTP 404 (no release published
// yet), malformed JSON, unparseable tag — routes to checkFailed, so callers can
// stay silent on the automatic startup path.
//
// Widget-free (lives in pj_runtime); the shell connects updateAvailable to
// whatever UI surface it wants (a toast, a menu badge, …).
class UpdateChecker : public QObject {
  Q_OBJECT

 public:
  explicit UpdateChecker(QObject* parent = nullptr);
  ~UpdateChecker() override;

  // Start the check. Any in-flight request is aborted first. Safe to call from
  // the GUI thread; the reply is handled asynchronously on the event loop.
  void checkLatestRelease();

  // Version this check compares the online tag against. Defaults (when empty)
  // to QCoreApplication::applicationVersion() at call time.
  void setCurrentVersion(const QString& version);

  // Override the releases API endpoint. Defaults to the PlotJuggler/PlotJuggler repo.
  void setReleaseApiUrl(const QUrl& url);

  // The releases API endpoint this check will GET (the built-in default, or the
  // last value passed to setReleaseApiUrl).
  QUrl releaseApiUrl() const;

 signals:
  // A strictly-newer release exists.
  void updateAvailable(const ReleaseInfo& release);

  // The running version is current (or newer than the published release).
  void upToDate();

  // The check could not be completed (network/HTTP/parse). `reason` is for logs.
  void checkFailed(const QString& reason);

 private:
  // Reply lifetime is owned by httpGetWithTimeout (deleteLater after return).
  void handleReply(QNetworkReply& reply);

  QNetworkAccessManager* network_ = nullptr;
  QNetworkReply* pending_reply_ = nullptr;
  QString current_version_;
  QUrl release_api_url_;
};

}  // namespace PJ

Q_DECLARE_METATYPE(PJ::ReleaseInfo)
