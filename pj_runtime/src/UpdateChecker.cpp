// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "pj_runtime/UpdateVersion.h"

namespace PJ {

namespace {
// GitHub's REST endpoint returns the latest published, non-draft,
// non-prerelease release — so betas never trigger the nag on their own.
constexpr auto kDefaultReleaseApiUrl = "https://api.github.com/repos/PlotJuggler/PJ4/releases/latest";
constexpr int kTransferTimeoutMs = 15000;
}  // namespace

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), release_api_url_(kDefaultReleaseApiUrl) {}

UpdateChecker::~UpdateChecker() = default;

void UpdateChecker::setCurrentVersion(const QString& version) {
  current_version_ = version;
}

void UpdateChecker::setReleaseApiUrl(const QUrl& url) {
  release_api_url_ = url;
}

void UpdateChecker::checkLatestRelease() {
  // Abandon any in-flight check. abort() makes that reply emit finished() with
  // OperationCanceledError, which handleReply() drops silently; and because each
  // handler captures its own `reply`, superseding a check can never make an old
  // reply's handler act on the new request.
  if (pending_reply_ && pending_reply_->isRunning()) {
    pending_reply_->abort();
  }

  QNetworkRequest request(release_api_url_);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  // api.github.com rejects requests without a User-Agent (HTTP 403).
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("PlotJuggler"));
  request.setRawHeader("Accept", "application/vnd.github+json");
  request.setTransferTimeout(kTransferTimeoutMs);

  QNetworkReply* reply = network_->get(request);
  pending_reply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    if (pending_reply_ == reply) {
      pending_reply_ = nullptr;
    }
    handleReply(reply);
  });
}

void UpdateChecker::handleReply(QNetworkReply* reply) {
  if (!reply) {
    emit checkFailed(QStringLiteral("no reply"));
    return;
  }

  // A self-inflicted abort (a newer check superseded this one) is not a
  // user-facing failure — drop it without emitting any outcome.
  if (reply->error() == QNetworkReply::OperationCanceledError) {
    reply->deleteLater();
    return;
  }

  // A 404 (no release published yet) surfaces here as ContentNotFoundError —
  // treated like any other failure, i.e. silently on the startup path.
  if (reply->error() != QNetworkReply::NoError) {
    emit checkFailed(reply->errorString());
    reply->deleteLater();
    return;
  }

  const QByteArray data = reply->readAll();
  reply->deleteLater();

  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    emit checkFailed(QStringLiteral("release JSON parse error: %1").arg(parse_error.errorString()));
    return;
  }
  if (!doc.isObject()) {
    emit checkFailed(QStringLiteral("release response was not a JSON object"));
    return;
  }

  const QJsonObject obj = doc.object();
  const QString tag_name = obj.value(QStringLiteral("tag_name")).toString();
  if (tag_name.isEmpty()) {
    emit checkFailed(QStringLiteral("release JSON missing tag_name"));
    return;
  }

  const QString current = current_version_.isEmpty() ? QCoreApplication::applicationVersion() : current_version_;

  if (!isNewerVersion(tag_name, current)) {
    emit upToDate();
    return;
  }

  const QString name = obj.value(QStringLiteral("name")).toString();
  const QString html_url = obj.value(QStringLiteral("html_url")).toString();
  emit updateAvailable(ReleaseInfo{name.isEmpty() ? tag_name : name, html_url});
}

}  // namespace PJ
