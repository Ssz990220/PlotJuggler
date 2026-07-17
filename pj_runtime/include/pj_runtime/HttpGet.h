#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file HttpGet.h
 * @brief One-shot async HTTP GET with a transfer timeout.
 *
 * The shared core of every "probe a URL and react to the outcome" caller
 * (UpdateChecker's release check, the Preferences registry-URL reachability
 * probe): issue the GET, deliver the finished reply to a callback on the
 * caller's thread, and own the reply's cleanup.
 */

#include <QNetworkRequest>
#include <chrono>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QObject;

namespace PJ {

// Fires one GET for `request` (caller-configured: URL, headers, redirect
// policy) with the given transfer timeout, and invokes on_finished with the
// finished reply on `context`'s thread — including on error, abort, and
// timeout. The reply is deleteLater'd right after the callback returns, so the
// callback must not retain it. Returns the in-flight reply (non-owning; it may
// be ignored) so callers that supersede checks can abort() it.
QNetworkReply* httpGetWithTimeout(
    QNetworkAccessManager& network, QNetworkRequest request, std::chrono::milliseconds timeout, QObject* context,
    std::function<void(QNetworkReply&)> on_finished);

}  // namespace PJ
