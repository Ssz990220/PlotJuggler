// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Behavioural contract for IngestProgressWidget — the non-modal title-bar
// progress strip. setActive() shows/hides and resets the action latch; clicking
// a configured action button emits actionRequested() and latches lastAction();
// the title + counter show in the bar's caption; an unconfigured button stays
// hidden; setRange(0, 0) is an indeterminate (busy) bar.

#include <gtest/gtest.h>

#include <QApplication>
#include <QProgressBar>
#include <QToolButton>

#include "pj_widgets/IngestProgressWidget.h"

namespace {

using PJ::IngestProgressWidget;
using Action = PJ::IngestProgressWidget::Action;

TEST(IngestProgressWidgetTest, StartsHiddenAndInactive) {
  IngestProgressWidget widget;
  EXPECT_TRUE(widget.isHidden());
  EXPECT_EQ(widget.lastAction(), Action::kNone);
}

TEST(IngestProgressWidgetTest, SetActiveShowsAndHides) {
  IngestProgressWidget widget;
  widget.setActive(true);
  EXPECT_FALSE(widget.isHidden());
  widget.setActive(false);
  EXPECT_TRUE(widget.isHidden());
}

TEST(IngestProgressWidgetTest, PrimaryClickEmitsAndLatches) {
  IngestProgressWidget widget;
  widget.setPrimaryButton(QStringLiteral("Keep"));

  int count = 0;
  Action got = Action::kNone;
  QObject::connect(&widget, &IngestProgressWidget::actionRequested, &widget, [&](Action action) {
    ++count;
    got = action;
  });

  auto* button = widget.findChild<QToolButton*>(QStringLiteral("ingestPrimaryButton"));
  ASSERT_NE(button, nullptr);
  button->click();

  EXPECT_EQ(count, 1);
  EXPECT_EQ(got, Action::kPrimary);
  EXPECT_EQ(widget.lastAction(), Action::kPrimary);
}

TEST(IngestProgressWidgetTest, SecondaryClickEmitsSecondary) {
  IngestProgressWidget widget;
  widget.setSecondaryButton(QStringLiteral("Discard"));

  Action got = Action::kNone;
  QObject::connect(&widget, &IngestProgressWidget::actionRequested, &widget, [&](Action action) { got = action; });

  auto* button = widget.findChild<QToolButton*>(QStringLiteral("ingestSecondaryButton"));
  ASSERT_NE(button, nullptr);
  button->click();
  EXPECT_EQ(got, Action::kSecondary);
  EXPECT_EQ(widget.lastAction(), Action::kSecondary);
}

TEST(IngestProgressWidgetTest, SetActiveFalseResetsLatch) {
  IngestProgressWidget widget;
  widget.setPrimaryButton(QStringLiteral("Keep"));
  widget.findChild<QToolButton*>(QStringLiteral("ingestPrimaryButton"))->click();
  ASSERT_EQ(widget.lastAction(), Action::kPrimary);
  widget.setActive(false);
  EXPECT_EQ(widget.lastAction(), Action::kNone);
}

TEST(IngestProgressWidgetTest, TitleAndCounterInCaption) {
  IngestProgressWidget widget;
  auto* bar = widget.findChild<QProgressBar*>(QStringLiteral("ingestProgressBar"));
  ASSERT_NE(bar, nullptr);
  widget.setTitle(QStringLiteral("data.mcap"));
  widget.setCounterText(QStringLiteral("2/5"));
  EXPECT_TRUE(bar->format().contains(QStringLiteral("data.mcap")));
  EXPECT_TRUE(bar->format().contains(QStringLiteral("2/5")));
  widget.setCounterText(QString());  // counter cleared, title stays
  EXPECT_FALSE(bar->format().contains(QStringLiteral("2/5")));
  EXPECT_TRUE(bar->format().contains(QStringLiteral("data.mcap")));
}

TEST(IngestProgressWidgetTest, UnconfiguredButtonStaysHidden) {
  IngestProgressWidget widget;
  auto* button = widget.findChild<QToolButton*>(QStringLiteral("ingestPrimaryButton"));
  ASSERT_NE(button, nullptr);
  EXPECT_FALSE(button->isVisibleTo(&widget));
  widget.setPrimaryButton(QStringLiteral("Keep"));
  EXPECT_TRUE(button->isVisibleTo(&widget));
  widget.setPrimaryButton(QString());  // empty label hides it again
  EXPECT_FALSE(button->isVisibleTo(&widget));
}

TEST(IngestProgressWidgetTest, IndeterminateRange) {
  IngestProgressWidget widget;
  auto* bar = widget.findChild<QProgressBar*>(QStringLiteral("ingestProgressBar"));
  ASSERT_NE(bar, nullptr);
  widget.setRange(0, 0);
  EXPECT_EQ(bar->minimum(), 0);
  EXPECT_EQ(bar->maximum(), 0);  // (0,0) => busy/indeterminate
  widget.setRange(0, 100);
  widget.setValue(42);
  EXPECT_EQ(bar->maximum(), 100);
  EXPECT_EQ(bar->value(), 42);
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);  // QWidget construction needs a GUI app
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
