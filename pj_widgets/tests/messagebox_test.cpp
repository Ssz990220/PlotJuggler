// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Display contract for PJ::MessageBox: a button label that is wider than the
// dialog's bounded width must wrap to multiple lines rather than clip, while a
// short label is left untouched (rendered identically to before). The dialog
// must always respect its maximum width.

#include "pj_widgets/MessageBox.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QList>
#include <QPoint>
#include <QPushButton>
#include <algorithm>

namespace {

// Force the button to be laid out so width()/sizeHint() reflect the final,
// wrapped geometry. show() triggers Polish (label wrapping) then the
// showEvent() resize; the second processEvents() lets that resize's relayout
// settle before we measure.
void realize(PJ::MessageBox& dlg) {
  dlg.show();
  QCoreApplication::processEvents();
  QCoreApplication::processEvents();
}

// Vertical gaps (px) between consecutive stacked buttons, ordered top→bottom.
QList<int> buttonGaps(PJ::MessageBox& dlg) {
  auto buttons = dlg.findChildren<QPushButton*>(QStringLiteral("pjMessageBoxButton"));
  std::sort(buttons.begin(), buttons.end(), [&dlg](QPushButton* a, QPushButton* b) {
    return a->mapTo(&dlg, QPoint(0, 0)).y() < b->mapTo(&dlg, QPoint(0, 0)).y();
  });
  QList<int> gaps;
  for (int i = 1; i < buttons.size(); ++i) {
    const int prev_bottom = buttons[i - 1]->mapTo(&dlg, QPoint(0, 0)).y() + buttons[i - 1]->height();
    gaps.push_back(buttons[i]->mapTo(&dlg, QPoint(0, 0)).y() - prev_bottom);
  }
  return gaps;
}

TEST(MessageBoxTest, ShortLabelIsNotWrapped) {
  PJ::MessageBox dlg;
  dlg.setTitle(QStringLiteral("Title"));
  dlg.setText(QStringLiteral("Body."));
  QPushButton* ok = dlg.addButton(QStringLiteral("OK"), PJ::MessageBox::kPrimaryRole);
  realize(dlg);
  // No soft break inserted for a label that already fits on one line.
  EXPECT_FALSE(ok->text().contains(QLatin1Char('\n')));
  EXPECT_EQ(ok->text(), QStringLiteral("OK"));
}

TEST(MessageBoxTest, LongLabelWrapsInsteadOfClipping) {
  PJ::MessageBox dlg;
  dlg.setTitle(QStringLiteral("Confirm"));
  dlg.setText(QStringLiteral("Are you sure?"));
  QPushButton* btn = dlg.addButton(
      QStringLiteral("Reload the original file from disk and discard all unsaved local edits permanently"),
      PJ::MessageBox::kPrimaryRole);
  dlg.addButton(QStringLiteral("Cancel"), PJ::MessageBox::kCancelRole);
  realize(dlg);

  // The dialog never exceeds its width cap.
  EXPECT_LE(dlg.width(), dlg.maximumWidth());
  // The long label is broken across lines...
  EXPECT_TRUE(btn->text().contains(QLatin1Char('\n')));
  // ...so the button no longer demands more width than it is actually given:
  // its minimum width hint fits within its laid-out width (i.e. not clipped).
  EXPECT_LE(btn->minimumSizeHint().width(), btn->width());
}

TEST(MessageBoxTest, WrapsCorrectlyUnderLargerStyledFont) {
  // Regression: the wrap budget must be measured with the button's *rendered*
  // (QSS-applied) font, not the default font. A stylesheet that enlarges the
  // font reproduces the bug where labels were wrapped with stale metrics and
  // the lines then overflowed (clipped) at the real, larger font size.
  const QString prev = qApp->styleSheet();
  qApp->setStyleSheet(QStringLiteral("QWidget { font-size: 16pt; }"));
  {
    PJ::MessageBox dlg;
    dlg.setText(QStringLiteral("Body."));
    QPushButton* btn = dlg.addButton(
        QStringLiteral("Reload the original file from disk and discard all unsaved local edits permanently"),
        PJ::MessageBox::kPrimaryRole);
    dlg.addButton(QStringLiteral("Cancel"), PJ::MessageBox::kCancelRole);
    realize(dlg);
    EXPECT_TRUE(btn->text().contains(QLatin1Char('\n')));
    EXPECT_LE(btn->minimumSizeHint().width(), btn->width());
  }
  qApp->setStyleSheet(prev);
}

TEST(MessageBoxTest, WrappedButtonGrowsTaller) {
  PJ::MessageBox dlg;
  dlg.setText(QStringLiteral("Body."));
  QPushButton* shortb = dlg.addButton(QStringLiteral("OK"), PJ::MessageBox::kPrimaryRole);
  QPushButton* longb = dlg.addButton(
      QStringLiteral("Reload the original file from disk and discard all unsaved local edits permanently"),
      PJ::MessageBox::kNeutralRole);
  realize(dlg);
  // Multi-line wrapping makes the long button visibly taller than the 1-line one.
  EXPECT_GT(longb->height(), shortb->height());
}

TEST(MessageBoxTest, ButtonSpacingIsConstantAcrossDialogs) {
  // A wrapping body used to over-constrain the layout, which then compressed
  // the gaps between buttons by a variable amount (measured 5–14 px across
  // dialogs). Every gap must now be a constant 6 px regardless of button
  // count or body length. The compression only reproduces under the app's QSS
  // metrics (10pt font + the button min-height/padding that make the dialog
  // tall enough to over-constrain), so apply a matching stylesheet here.
  constexpr int kExpectedGap = 6;
  const QString prev = qApp->styleSheet();
  qApp->setStyleSheet(QStringLiteral(
      "QWidget { font-size: 10pt; }"
      "QPushButton#pjMessageBoxButton { min-height: 26px; padding: 6px 12px; border: none; }"));

  PJ::MessageBox three;
  three.setTitle(QStringLiteral("Load Layout"));
  three.setText(QStringLiteral(
      "This layout was saved with 1 data source(s):\n"
      "  /home/davide/ws_plotjuggler/DATA/example-024-quadruped-ds.mcap\n\n"
      "Reload them, or apply the layout to the currently loaded data?"));
  three.addButton(QStringLiteral("Reload original"), PJ::MessageBox::kPrimaryRole);
  three.addButton(QStringLiteral("Use current data"), PJ::MessageBox::kNeutralRole);
  three.addButton(QStringLiteral("Cancel"), PJ::MessageBox::kCancelRole);
  realize(three);
  const QList<int> three_gaps = buttonGaps(three);
  ASSERT_EQ(three_gaps.size(), 2);
  for (int gap : three_gaps) {
    EXPECT_EQ(gap, kExpectedGap);
  }

  PJ::MessageBox two;
  two.setText(QStringLiteral("Are you sure you want to remove 'example-024-quadruped-ds.mcap' and its data?"));
  two.addButton(QStringLiteral("Remove"), PJ::MessageBox::kDestructiveRole);
  two.addButton(QStringLiteral("Cancel"), PJ::MessageBox::kCancelRole);
  realize(two);
  const QList<int> two_gaps = buttonGaps(two);
  ASSERT_EQ(two_gaps.size(), 1);
  EXPECT_EQ(two_gaps.front(), kExpectedGap);

  qApp->setStyleSheet(prev);
}

}  // namespace

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");  // don't override a platform set by CI
  }
  QApplication app(argc, argv);  // QWidget + fontMetrics need a GUI app
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
