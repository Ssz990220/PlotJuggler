// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Regression: a multi-line, word-wrapped MessageBox body must not clip. The
// dialog used to open one line tall (the body label's heightForWidth flag was
// cleared and the styled card breaks heightForWidth propagation), so verbose
// question() bodies — e.g. the destructive dataset-merge confirmation — were cut
// off. The fix grows the dialog to the wrapped height on first show.

#include <gtest/gtest.h>

#include <QApplication>
#include <QFontMetrics>
#include <QLabel>

#include "pj_widgets/MessageBox.h"

namespace {

// The body label is reachable by its objectName (no test-only accessor needed).
QLabel* bodyLabelOf(PJ::MessageBox& box) {
  return box.findChild<QLabel*>(QStringLiteral("pjMessageBoxBody"));
}

TEST(MessageBoxTest, MultiLineBodyIsNotClipped) {
  PJ::MessageBox dlg;
  dlg.setTitle(QStringLiteral("Merge datasets"));
  dlg.setText(QStringLiteral(
      "Merging datasets is a destructive operation. Do you wish to proceed?\n\n"
      "Datasets run1, run2 overlap in time.\n"
      "Datasets run1, run2 have colliding data.\n"
      "Datasets run1, run2 contain object topics that will be dropped."));
  dlg.addButton(QStringLiteral("Merge"), PJ::MessageBox::kDestructiveRole);
  dlg.addButton(QStringLiteral("Cancel"), PJ::MessageBox::kCancelRole);

  dlg.show();
  QApplication::processEvents();

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  // The label is at least as tall as its wrapped text needs at this width —
  // i.e. nothing is clipped.
  EXPECT_GE(body->height(), body->heightForWidth(body->width()))
      << "body height " << body->height() << " < wrapped height " << body->heightForWidth(body->width());
  // And it genuinely wrapped to several lines (sanity that we exercised the path).
  const QFontMetrics fm(body->font());
  EXPECT_GT(body->height(), fm.height() * 3);
  dlg.close();
}

TEST(MessageBoxTest, ShortBodyAlsoFits) {
  PJ::MessageBox dlg;
  dlg.setTitle(QStringLiteral("Heads up"));
  dlg.setText(QStringLiteral("All good."));
  dlg.addButton(QStringLiteral("OK"), PJ::MessageBox::kPrimaryRole);

  dlg.show();
  QApplication::processEvents();

  QLabel* body = bodyLabelOf(dlg);
  ASSERT_NE(body, nullptr);
  ASSERT_GT(body->width(), 0);
  EXPECT_GE(body->height(), body->heightForWidth(body->width()));
  dlg.close();
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);  // QWidget construction + layout need a GUI app
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
