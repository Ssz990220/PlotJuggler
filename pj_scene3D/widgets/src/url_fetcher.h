// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared async URL/file fetcher for the scene layers (robot-model kUrl source,
// SceneEntities ModelPrimitive URLs). Replaces the nested-QEventLoop blocking
// fetches those layers used to spin from attach()/render() paths.

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

namespace pj::scene3d {

// Outcome of one UrlFetcher::fetch. `bytes` is meaningful only when `ok`.
struct FetchResult {
  bool ok{false};
  QByteArray bytes;
  QString error;
};

// Asynchronous byte fetcher with a hard size cap and bounded redirects.
//
// Contract (all of it load-bearing):
//  - GUI-thread only: construct, fetch(), and destroy on the thread whose event
//    loop delivers the callbacks.
//  - The callback is ALWAYS delivered asynchronously through the event loop —
//    even for local files and immediate errors (deferred via a queued
//    single-shot) — so a caller never re-enters its own stack from fetch().
//  - Local sources (file:// URLs and scheme-less bare paths) are read with
//    QFile inside the fetcher; only http/https ever reach the network; any
//    other scheme fails with "unsupported URL scheme".
//  - Network requests use QNetworkRequest::setTransferTimeout (15 s) and
//    NoLessSafeRedirectPolicy capped at 4 redirects — bounded, with no
//    https->http downgrade. That policy (rather than per-hop re-validation) is
//    deliberate: callers gate remote fetch by all-or-nothing consent, not by a
//    host allowlist, so there is no list a redirect could bypass.
//  - The response is aborted once it exceeds kMaxFetchBytes — checked against
//    the declared Content-Length as soon as the headers arrive, and against
//    the buffered byte count as the body streams in.
//  - Destroying the fetcher aborts every in-flight request and drops its
//    callback: a callback can never fire after the fetcher is gone. Layers
//    rely on this — each owns its fetcher, so layer destruction cancels the
//    fetch; any extra QPointer guard in a callback is insurance only.
class UrlFetcher : public QObject {
  Q_OBJECT
 public:
  // Hard cap on accepted response bytes (64 MiB): a hostile server cannot OOM
  // the app by streaming gigabytes within the transfer timeout.
  static constexpr qint64 kMaxFetchBytes = 64LL * 1024 * 1024;

  explicit UrlFetcher(QObject* parent = nullptr);

  // Start one fetch. `on_done` is invoked exactly once, via the event loop,
  // unless the fetcher is destroyed first (then never).
  void fetch(const QUrl& url, std::function<void(FetchResult)> on_done);

 private:
  // Queue `result` to `on_done` through the event loop — the never-synchronous
  // delivery guarantee for the local-file / immediate-error paths. Parented to
  // `this`, so destruction drops pending deliveries too.
  void deliverLater(std::function<void(FetchResult)> on_done, FetchResult result);

  QNetworkAccessManager manager_;
};

}  // namespace pj::scene3d
