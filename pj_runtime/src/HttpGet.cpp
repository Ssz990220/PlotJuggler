// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/HttpGet.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <utility>

namespace PJ {

QNetworkReply* httpGetWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(QNetworkReply&)> on_finished) {
  request.setTransferTimeout(static_cast<int>(timeout.count()));
  QNetworkReply* reply = network.get(request);
  QObject::connect(reply, &QNetworkReply::finished, context, [reply, callback = std::move(on_finished)]() {
    callback(*reply);
    reply->deleteLater();
  });
  return reply;
}

}  // namespace PJ
