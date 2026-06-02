// SPDX-License-Identifier: MPL-2.0
#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QtTest/QtTest>

#include "pj_widgets/RasterStreamView.h"

namespace {

bool hasPaintedStubFrame(PJ::RasterStreamView& view) {
  QImage rendered(view.size(), QImage::Format_RGB32);
  rendered.fill(Qt::black);

  QPainter painter(&rendered);
  view.render(&painter);

  const QColor center = rendered.pixelColor(rendered.rect().center());
  return center.red() > 0 || center.green() > 0 || center.blue() > 0;
}

// Launch the stub helper through the view; it publishes one frame. Verify the
// view connects, paints the copied frame, and does not prematurely end the session.
TEST(RasterStreamView, ReceivesAStubFrameAndStaysAlive) {
  PJ::RasterStreamView view;
  view.resize(320, 200);
  QSignalSpy ended(&view, &PJ::RasterStreamView::sessionEnded);
  view.start(QStringLiteral(PJ_STUB_HELPER_PATH), QStringLiteral("/dev/null"));

  bool received_frame = false;
  for (int attempt = 0; attempt < 60 && !received_frame; ++attempt) {
    QTest::qWait(50);
    received_frame = hasPaintedStubFrame(view);
  }

  EXPECT_TRUE(received_frame);
  EXPECT_EQ(ended.count(), 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
