// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// UrlFetcher contract tests: always-asynchronous delivery (local files
// included), scheme gating, the no-callback-after-destruction guarantee, and
// the response size cap. Network paths are exercised against an in-process
// QTcpServer so nothing here touches a real network.

#include "url_fetcher.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <functional>
#include <memory>

namespace {

// Pump the event loop until `done` returns true or `timeout_ms` elapses.
bool pumpUntil(const std::function<bool()>& done, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!done() && timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}

QString writeTempFile(const QTemporaryDir& dir, const char* name, const QByteArray& bytes) {
  const QString path = dir.filePath(QLatin1String(name));
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
  return path;
}

}  // namespace

TEST(UrlFetcherTest, LocalFileUrlDeliversBytesAsynchronously) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QByteArray payload("hello mesh bytes");
  const QString path = writeTempFile(dir, "payload.bin", payload);

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl::fromLocalFile(path), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  // The contract: the callback is NEVER invoked synchronously from fetch() —
  // a caller must be safe kicking a fetch from inside its own load path.
  EXPECT_FALSE(called) << "callback ran before fetch() returned";

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, payload);
}

TEST(UrlFetcherTest, SchemelessBarePathReadsLocalFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QByteArray payload("bare path bytes");
  const QString path = writeTempFile(dir, "bare.bin", payload);

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(QUrl(path), [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_TRUE(result.ok) << result.error.toStdString();
  EXPECT_EQ(result.bytes, payload);
}

TEST(UrlFetcherTest, MissingLocalFileFailsWithError) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl::fromLocalFile(QStringLiteral("/nonexistent/url_fetcher_test/missing.bin")),
      [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      });
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.isEmpty());
}

TEST(UrlFetcherTest, UnsupportedSchemeFailsAsynchronously) {
  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  fetcher.fetch(
      QUrl(QStringLiteral("ftp://example.invalid/mesh.stl")), [&called, &result](pj::scene3d::FetchResult fetched) {
        called = true;
        result = std::move(fetched);
      });
  EXPECT_FALSE(called) << "even immediate errors must be delivered through the event loop";
  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 5000));
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains(QStringLiteral("unsupported URL scheme"))) << result.error.toStdString();
}

// The lifetime guarantee layers rely on: destroying the fetcher while a request
// is in flight aborts it and the callback never fires.
TEST(UrlFetcherTest, DestructionDropsInFlightCallback) {
  // A server that accepts the connection but never responds keeps the request
  // in flight for as long as the test wants.
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));

  auto fetcher = std::make_unique<pj::scene3d::UrlFetcher>();
  bool called = false;
  const QUrl url(QStringLiteral("http://127.0.0.1:%1/never").arg(server.serverPort()));
  fetcher->fetch(url, [&called](pj::scene3d::FetchResult) { called = true; });

  // Let the connection actually establish, then kill the fetcher mid-flight.
  pumpUntil([&server]() { return server.hasPendingConnections(); }, 2000);
  fetcher.reset();

  pumpUntil([]() { return false; }, 300);  // give a stray queued callback time to fire
  EXPECT_FALSE(called) << "callback fired after the fetcher was destroyed";
}

// Size cap: a response whose declared Content-Length exceeds kMaxFetchBytes is
// aborted instead of buffered.
TEST(UrlFetcherTest, ResponseBeyondSizeCapIsAborted) {
  QTcpServer server;
  ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
    QTcpSocket* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      socket->readAll();  // consume the request; the content is irrelevant
      // 100 MiB declared > the 64 MiB cap: the fetcher must abort on the
      // headers (metaDataChanged) without waiting for the body to stream.
      socket->write("HTTP/1.1 200 OK\r\nContent-Length: 104857600\r\n\r\n");
      socket->write(QByteArray(4096, 'x'));
      socket->flush();
    });
  });

  pj::scene3d::UrlFetcher fetcher;
  bool called = false;
  pj::scene3d::FetchResult result;
  const QUrl url(QStringLiteral("http://127.0.0.1:%1/huge").arg(server.serverPort()));
  fetcher.fetch(url, [&called, &result](pj::scene3d::FetchResult fetched) {
    called = true;
    result = std::move(fetched);
  });

  ASSERT_TRUE(pumpUntil([&called]() { return called; }, 10000)) << "capped fetch never completed";
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.error.contains(QStringLiteral("limit"))) << result.error.toStdString();
}

// Custom main: the QCoreApplication must die BEFORE exit handlers run — QtNetwork
// registers global cleanup that a function-local-static app would outlive,
// crashing at exit (same pattern as pj_marketplace's download_manager_test).
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
