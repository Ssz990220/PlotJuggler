// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <chrono>
#include <utility>

#include "pj_runtime/HttpGet.h"
#include "pj_runtime/UpdateVersion.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// GitHub's REST endpoint returns the latest published, non-draft,
// non-prerelease release — so betas never trigger the nag on their own.
constexpr auto kDefaultReleaseApiUrl = "https://api.github.com/repos/PlotJuggler/PlotJuggler/releases/latest";
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

QUrl UpdateChecker::releaseApiUrl() const {
  return release_api_url_;
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
  request.setHeader(QNetworkRequest::UserAgentHeader, u"PlotJuggler"_s);
  request.setRawHeader("Accept", "application/vnd.github+json");

  pending_reply_ = httpGetWithTimeout(
      *network_, std::move(request), std::chrono::milliseconds(kTransferTimeoutMs), this, [this](QNetworkReply& reply) {
        if (pending_reply_ == &reply) {
          pending_reply_ = nullptr;
        }
        handleReply(reply);
      });
}

void UpdateChecker::handleReply(QNetworkReply& reply) {
  // A self-inflicted abort (a newer check superseded this one) is not a
  // user-facing failure — drop it without emitting any outcome.
  if (reply.error() == QNetworkReply::OperationCanceledError) {
    return;
  }

  // A 404 (no release published yet) surfaces here as ContentNotFoundError —
  // treated like any other failure, i.e. silently on the startup path.
  if (reply.error() != QNetworkReply::NoError) {
    emit checkFailed(reply.errorString());
    return;
  }

  const QByteArray data = reply.readAll();

  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    emit checkFailed(u"release JSON parse error: %1"_s.arg(parse_error.errorString()));
    return;
  }
  if (!doc.isObject()) {
    emit checkFailed(u"release response was not a JSON object"_s);
    return;
  }

  const QJsonObject obj = doc.object();
  const QString tag_name = obj.value(u"tag_name"_s).toString();
  if (tag_name.isEmpty()) {
    emit checkFailed(u"release JSON missing tag_name"_s);
    return;
  }

  const QString current = current_version_.isEmpty() ? QCoreApplication::applicationVersion() : current_version_;

  if (!isNewerVersion(tag_name, current)) {
    emit upToDate();
    return;
  }

  const QString name = obj.value(u"name"_s).toString();
  const QString html_url = obj.value(u"html_url"_s).toString();
  emit updateAvailable(ReleaseInfo{name.isEmpty() ? tag_name : name, html_url});
}

}  // namespace PJ
