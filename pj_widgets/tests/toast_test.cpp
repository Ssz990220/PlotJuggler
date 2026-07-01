// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QLabel>
#include <QWidget>

#include "pj_widgets/ToastManager.h"
#include "pj_widgets/ToastNotification.h"

namespace {

// QSS type selectors match on metaObject()->className(). The toast styling in
// stylesheet_{dark,light}.qss targets `PJ--ToastNotification`, so pin the class
// name: a namespace/rename regression would silently drop the styling instead
// of failing loudly.
TEST(ToastTest, ClassNameDrivesQssSelector) {
  PJ::ToastNotification toast("hello");
  EXPECT_STREQ(toast.metaObject()->className(), "PJ::ToastNotification");
}

// showToast() must create a ToastNotification under the manager's transparent
// container (objectName preserved from PJ3 for stylesheet selectors).
TEST(ToastTest, ShowToastCreatesNotification) {
  QWidget host;
  host.resize(800, 600);
  PJ::ToastManager manager(&host);

  // timeout_ms == 0 disables auto-dismiss so nothing tears down mid-test.
  manager.showToast("plain message", QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* container = host.findChild<QWidget*>("toastManagerContainer");
  ASSERT_NE(container, nullptr);
  EXPECT_NE(host.findChild<PJ::ToastNotification*>(), nullptr);
}

// The message label is rich text with external links enabled — this is what
// makes an `<a href>` in the message open in the system browser with no extra
// wiring (the release-check "View on GitHub" link relies on it).
TEST(ToastTest, MessageLabelIsRichTextWithClickableLink) {
  QWidget host;
  host.resize(800, 600);
  PJ::ToastManager manager(&host);

  const QString message = R"(New release: <a href="https://github.com/PlotJuggler/PJ4">View</a>)";
  manager.showToast(message, QPixmap(), 0);
  QCoreApplication::processEvents();

  auto* toast = host.findChild<PJ::ToastNotification*>();
  ASSERT_NE(toast, nullptr);
  EXPECT_EQ(toast->message(), message);

  auto* label = toast->findChild<QLabel*>("toastMessage");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->textFormat(), Qt::RichText);
  EXPECT_TRUE(label->openExternalLinks());
  EXPECT_TRUE(label->text().contains("github.com/PlotJuggler/PJ4"));
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);  // QWidget construction needs a GUI app
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
