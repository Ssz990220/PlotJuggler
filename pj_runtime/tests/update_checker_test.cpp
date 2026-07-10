// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Exercises UpdateChecker's reply state machine against a local HTTP server, so
// the "emit exactly one of updateAvailable/upToDate/checkFailed" contract, the
// name-fallback, the 404/parse/missing-tag failure routing, and the
// abort-on-supersede behavior are verified without touching real GitHub.
// setCurrentVersion() makes the comparison deterministic (the test binary has
// no PJ_VERSION_STRING, so applicationVersion() would otherwise be empty).

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include "pj_runtime/UpdateChecker.h"
using namespace Qt::StringLiterals;

namespace {

// Spins the event loop until `spy` receives a signal or the timeout expires.
bool waitForSignal(QSignalSpy& spy, int timeout_ms = 5000) {
  QDeadlineTimer deadline(timeout_ms);
  while (spy.isEmpty() && !deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return !spy.isEmpty();
}

// Minimal loopback HTTP/1.1 server that replies to every request with a
// configurable status line + body. No external network required.
class LocalHttpServer {
 public:
  LocalHttpServer() {
    server_.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
      QTcpSocket* socket = server_.nextPendingConnection();
      socket->setParent(&server_);
      QObject::connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        socket->readAll();  // consume the request
        socket->write(response_);
        socket->flush();
        socket->disconnectFromHost();
      });
    });
  }

  QUrl url() const {
    return QUrl(u"http://127.0.0.1:%1/"_s.arg(server_.serverPort()));
  }

  void setResponse(const QByteArray& status_line, const QByteArray& body) {
    response_ = "HTTP/1.1 " + status_line +
                "\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " +
                QByteArray::number(body.size()) +
                "\r\n"
                "Connection: close\r\n\r\n" +
                body;
  }

 private:
  QTcpServer server_;
  QByteArray response_;
};

}  // namespace

TEST(UpdateCheckerTest, NewerReleaseEmitsUpdateAvailable) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.0.0","name":"PJ 4","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.count(), 1);
  EXPECT_TRUE(uptodate_spy.isEmpty());
  EXPECT_TRUE(failed_spy.isEmpty());

  const auto release = update_spy.first().at(0).value<PJ::ReleaseInfo>();
  EXPECT_EQ(release.name, u"PJ 4"_s);
  EXPECT_EQ(release.html_url, u"https://x/rel"_s);
}

TEST(UpdateCheckerTest, EmptyNameFallsBackToTag) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"4.0.0","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.first().at(0).value<PJ::ReleaseInfo>().name, u"4.0.0"_s);
}

TEST(UpdateCheckerTest, EqualVersionIsUpToDate) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"3.999.0"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(uptodate_spy));
  EXPECT_EQ(uptodate_spy.count(), 1);
  EXPECT_TRUE(update_spy.isEmpty());
  EXPECT_TRUE(failed_spy.isEmpty());
}

TEST(UpdateCheckerTest, OlderReleaseIsUpToDate) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"tag_name":"3.998.0"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  checker.checkLatestRelease();
  EXPECT_TRUE(waitForSignal(uptodate_spy));
}

TEST(UpdateCheckerTest, Http404EmitsCheckFailedSilently) {
  LocalHttpServer server;
  server.setResponse("404 Not Found", R"({"message":"Not Found"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  EXPECT_EQ(failed_spy.count(), 1);
  // The whole "dormant until first release" design rests on 404 NOT looking
  // like an update or an up-to-date result.
  EXPECT_TRUE(update_spy.isEmpty());
  EXPECT_TRUE(uptodate_spy.isEmpty());
}

TEST(UpdateCheckerTest, ValidNonObjectJsonEmitsDistinctFailure) {
  LocalHttpServer server;
  server.setResponse("200 OK", "[]");  // valid JSON, wrong shape

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);
  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  const QString reason = failed_spy.first().at(0).toString();
  // Must not misreport a valid-but-non-object body as a "parse error: no error".
  EXPECT_TRUE(reason.contains(u"not a JSON object"_s));
}

TEST(UpdateCheckerTest, MissingTagNameEmitsCheckFailed) {
  LocalHttpServer server;
  server.setResponse("200 OK", R"({"name":"no tag here"})");

  PJ::UpdateChecker checker;
  checker.setReleaseApiUrl(server.url());
  checker.setCurrentVersion(u"3.999.0"_s);

  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.checkLatestRelease();

  ASSERT_TRUE(waitForSignal(failed_spy));
  EXPECT_TRUE(update_spy.isEmpty());
}

TEST(UpdateCheckerTest, SupersededCheckEmitsNothing) {
  // First check points at a server that accepts the connection but never
  // replies; the second check supersedes (and aborts) it.
  QTcpServer hanging;
  hanging.listen(QHostAddress::LocalHost, 0);

  LocalHttpServer good;
  good.setResponse("200 OK", R"({"tag_name":"4.0.0","name":"PJ 4","html_url":"https://x/rel"})");

  PJ::UpdateChecker checker;
  checker.setCurrentVersion(u"3.999.0"_s);
  QSignalSpy update_spy(&checker, &PJ::UpdateChecker::updateAvailable);
  QSignalSpy uptodate_spy(&checker, &PJ::UpdateChecker::upToDate);
  QSignalSpy failed_spy(&checker, &PJ::UpdateChecker::checkFailed);

  checker.setReleaseApiUrl(QUrl(u"http://127.0.0.1:%1/"_s.arg(hanging.serverPort())));
  checker.checkLatestRelease();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 100);

  checker.setReleaseApiUrl(good.url());
  checker.checkLatestRelease();  // aborts the hanging request

  ASSERT_TRUE(waitForSignal(update_spy));
  EXPECT_EQ(update_spy.count(), 1);
  EXPECT_TRUE(uptodate_spy.isEmpty());
  // The aborted (superseded) request must not surface as a failure.
  EXPECT_TRUE(failed_spy.isEmpty());
}

TEST(UpdateCheckerTest, DefaultUrlTargetsPublicRepo) {
  // The check must hit the PUBLIC PlotJuggler/PlotJuggler repo: the private
  // PlotJuggler/PJ4 dev repo is invisible to anonymous clients, so aiming the
  // default there makes every check fail (403 rate-limit / 404 not-found).
  PJ::UpdateChecker checker;
  const std::string url = checker.releaseApiUrl().toString().toStdString();
  EXPECT_NE(url.find("api.github.com"), std::string::npos) << url;
  EXPECT_NE(url.find("/repos/PlotJuggler/PlotJuggler/releases/latest"), std::string::npos) << url;
  EXPECT_EQ(url.find("/PlotJuggler/PJ4/"), std::string::npos) << url;
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  qRegisterMetaType<PJ::ReleaseInfo>();  // so QSignalSpy can capture updateAvailable's arg
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
