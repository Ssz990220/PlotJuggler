// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/Scrollbar.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QWheelEvent>

using PJ::scrollbar_detail::computeHandle;
using PJ::scrollbar_detail::defaultAccent;
using PJ::scrollbar_detail::Handle;
using PJ::scrollbar_detail::kPillAccentDark;
using PJ::scrollbar_detail::kPillAccentLight;
using PJ::scrollbar_detail::valueForDrag;

// ---------------------------------------------------------------------------
// Headless math tests — no QApplication needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarHandle, FullRangeStartsAtZero) {
  // 0..100, page 100, value 0 over a 200px track -> full-length handle at 0.
  Handle h = computeHandle(0, 100, 0, 100, 200.0);
  EXPECT_NEAR(h.pos, 0.0, 1e-6);
  EXPECT_NEAR(h.len, 100.0, 1e-6);  // page/(range+page)=100/200 * 200
}

TEST(ScrollbarHandle, MidValueOffsetsHandle) {
  Handle h = computeHandle(0, 100, 50, 100, 200.0);
  EXPECT_NEAR(h.len, 100.0, 1e-6);
  EXPECT_NEAR(h.pos, 50.0, 1e-6);  // frac 50/200 * 200, clamped within track
}

TEST(ScrollbarHandle, TinyPageFloorsToMinLen) {
  Handle h = computeHandle(0, 10000, 0, 1, 200.0);
  EXPECT_GE(h.len, 28.0);  // kMinPillLenPx floor keeps it grabbable
}

TEST(ScrollbarHandle, NothingToScrollIsEmpty) {
  Handle h = computeHandle(0, 0, 0, 100, 200.0);
  EXPECT_NEAR(h.len, 0.0, 1e-6);
}

TEST(ScrollbarHandle, ZeroPageStepIsEmpty) {
  // Degenerate: max>min but pageStep 0 (e.g. a collapsed viewport) → no real
  // handle, so paint nothing instead of flooring a phantom kMinPillLenPx pill.
  Handle h = computeHandle(0, 100, 0, 0, 200.0);
  EXPECT_NEAR(h.len, 0.0, 1e-6);
}

// ---------------------------------------------------------------------------
// defaultAccent — theme-aware pill color keyed off QPalette::Window lightness
// (NOT the system QPalette::Highlight, which the app never sets). No QApplication
// needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarAccent, DarkWindowYieldsPaleGrey) {
  // A dark chrome (low lightness) must pick the pale-grey pill, never the OS
  // highlight (e.g. Yaru orange).
  EXPECT_EQ(defaultAccent(QColor(0x2B, 0x2B, 0x33)), kPillAccentDark);
}

TEST(ScrollbarAccent, LightWindowYieldsInfoBlue) {
  EXPECT_EQ(defaultAccent(QColor(0xF5, 0xF5, 0xF5)), kPillAccentLight);
}

// ---------------------------------------------------------------------------
// valueForDrag math tests — no QApplication needed.
// ---------------------------------------------------------------------------

TEST(ScrollbarDrag, HalfTrackDeltaMapsToHalfTotal) {
  // delta of half the track (100px) over total=200, start=0 → value moves by 100.
  EXPECT_EQ(valueForDrag(0, 100.0, 200.0, 200), 100);
}

TEST(ScrollbarDrag, ZeroTrackLenReturnsStart) {
  EXPECT_EQ(valueForDrag(50, 10.0, 0.0, 200), 50);
}

TEST(ScrollbarDrag, NegativeDeltaScrollsBack) {
  EXPECT_EQ(valueForDrag(100, -50.0, 200.0, 200), 50);
}

TEST(ScrollbarDrag, NonZeroStartValueOffsets) {
  // start=10, delta 1/4 track, total=100 → start + 25 = 35.
  EXPECT_EQ(valueForDrag(10, 50.0, 200.0, 100), 35);
}

// ---------------------------------------------------------------------------
// Widget tests — require QApplication.
// ---------------------------------------------------------------------------

namespace {

// One QApplication for the whole binary; QWidget construction requires it.
struct QtEnvironment : ::testing::Environment {
  void SetUp() override {
    static int argc = 0;
    app_ = new QApplication(argc, nullptr);
  }
  void TearDown() override {
    delete app_;
    app_ = nullptr;
  }
  QApplication* app_ = nullptr;
};

const auto* kQtEnv = ::testing::AddGlobalTestEnvironment(new QtEnvironment);

}  // namespace

// ---------------------------------------------------------------------------
// Config tests — auto-hide + fade duration.
// ---------------------------------------------------------------------------

TEST(ScrollbarConfig, AutoHideOffStaysVisible) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);
  bar.setAutoHide(false);
  QTest::qWait(50);
  EXPECT_DOUBLE_EQ(bar.currentOpacity(), 1.0);
}

TEST(ScrollbarConfig, InstantFadeWhenDurationZero) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.setFadeDurationMs(0);
  bar.attach(&area);

  // (1) The configured duration must propagate to the underlying animation.
  EXPECT_EQ(bar.fadeDurationMs(), 0);

  // (2) With duration=0 a show must complete synchronously — opacity reaches
  // 1.0 within the setAutoHide(false) call frame, with NO qWait() in between.
  // This exercises the instant-snap code path in setShown(): with fade_ms_==0
  // the opacity_effect_ is set directly rather than scheduling an animation
  // timer tick (QPropertyAnimation defers even duration=0 to the event loop).
  bar.setAutoHide(false);
  EXPECT_DOUBLE_EQ(bar.currentOpacity(), 1.0);
}

TEST(ScrollbarWidget, AttachEnablesViewportMouseTracking) {
  // Without mouse tracking the viewport only delivers MouseMove while a button
  // is held, so the pill never fades in on a plain hover (it only appears on
  // click and never re-hides on intra-viewport motion). attach() must enable
  // tracking itself rather than relying on the host having done so (the Timeline
  // happens to; a generic plugin scroll area does not).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  ASSERT_FALSE(area.viewport()->hasMouseTracking()) << "precondition: viewport starts without tracking";

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);

  EXPECT_TRUE(area.viewport()->hasMouseTracking())
      << "attach() must enable viewport mouse tracking so hover-reveal works";
}

TEST(ScrollbarWidget, WheelScrollRevealsMatchingAxisOnly) {
  // A scroll (wheel / trackpad gesture) reveals the pill even when the cursor is
  // nowhere near the strip — and only the axis that actually scrolled reveals.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);  // both axes have something to scroll
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar h(Qt::Horizontal);
  h.attach(&area);
  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  ASSERT_FALSE(h.isShown());
  ASSERT_FALSE(v.isShown());

  // A purely-vertical wheel over the middle of the viewport (not the strip).
  const QPointF pos(100, 100);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QWheelEvent wheel(
      pos, global, QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);

  EXPECT_TRUE(v.isShown()) << "vertical wheel must reveal the vertical pill";
  EXPECT_FALSE(h.isShown()) << "vertical wheel must NOT reveal the horizontal pill";
}

TEST(ScrollbarWidget, ScrollRevealSurvivesHoverOut) {
  // Regression: a scroll reveals the pill, then a hover-out (e.g. the synthetic
  // MouseMove Qt delivers as scrolled content slides under a stationary cursor,
  // now that attach() enables mouse tracking) must NOT cancel the reveal — the
  // hold timer owns the hide. Previously this hid the pill instantly on scroll.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const QPointF mid(100, 100);  // in the content, NOT the right-edge strip
  const QPointF global = area.viewport()->mapToGlobal(mid);
  QWheelEvent wheel(
      mid, global, QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);
  ASSERT_TRUE(v.isShown()) << "scroll must reveal the pill";

  // A hover move in the middle (outside the strip) while the reveal hold is active.
  QMouseEvent move(QEvent::MouseMove, mid, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &move);

  EXPECT_TRUE(v.isShown()) << "a hover-out must not cancel an active scroll reveal";
}

TEST(ScrollbarWidget, PressOffHandleDoesNotScrollOrGrab) {
  // Regression: only the visible handle is a drag target. A press in the edge
  // strip but off the handle must fall through to the view — it must NOT be
  // consumed as a click-to-center jump (which would both steal the content click
  // and move the scroll position).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  QScrollBar* bar = area.verticalScrollBar();
  bar->setValue(bar->minimum());  // handle sits at the top of the track

  // Press mid-strip — in the right-edge strip, off the handle (which is at the
  // top), and clear of the bottom-right corner (yielded to the horizontal pill).
  const QPointF pos(area.viewport()->width() - 3, area.viewport()->height() / 2);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &press);

  EXPECT_EQ(bar->value(), bar->minimum()) << "an off-handle press must not jump the scroll value";
}

TEST(ScrollbarWidget, ClickToScrollJumpsOnOffHandlePress) {
  // With click-to-scroll opted in (the Timeline's mode), an off-handle press in
  // the strip centers the handle under the cursor, jumping the scroll value.
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.setClickToScroll(true);
  v.attach(&area);
  QScrollBar* bar = area.verticalScrollBar();
  bar->setValue(bar->minimum());  // handle at the top

  // Mid-strip, off the handle, clear of the yielded bottom-right corner.
  const QPointF pos(area.viewport()->width() - 3, area.viewport()->height() / 2);
  const QPointF global = area.viewport()->mapToGlobal(pos);
  QMouseEvent press(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &press);

  EXPECT_GT(bar->value(), bar->minimum()) << "click-to-scroll must jump toward an off-handle press";
}

TEST(ScrollbarWidget, WheelDoesNotRevealWhenLocked) {
  // A locked (non-interactive) scrollbar must not reveal on scroll — the view is
  // frozen, so a flashing pill would be a misleading affordance (finding #6).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);
  v.setInteractive(false);

  const QPointF pos(100, 100);
  QWheelEvent wheel(
      pos, area.viewport()->mapToGlobal(pos), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);

  EXPECT_FALSE(v.isShown()) << "a locked scrollbar must not reveal on scroll";
}

TEST(ScrollbarWidget, LockRetreatsShownPill) {
  // Locking mid-reveal must retreat the pill, not leave it stuck visible over the
  // frozen view (finding #8).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const QPointF pos(100, 100);
  QWheelEvent wheel(
      pos, area.viewport()->mapToGlobal(pos), QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
      Qt::NoScrollPhase, /*inverted=*/false);
  QCoreApplication::sendEvent(area.viewport(), &wheel);
  ASSERT_TRUE(v.isShown());

  v.setInteractive(false);
  EXPECT_FALSE(v.isShown()) << "locking must retreat a shown pill";
}

TEST(ScrollbarWidget, VerticalStripYieldsBottomRightCornerToHorizontal) {
  // The H and V strips overlap in the bottom-right corner; the vertical pill must
  // yield it so the horizontal pill's right end stays reachable (finding #4).
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);  // both axes scroll
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar h(Qt::Horizontal);
  h.attach(&area);
  PJ::Scrollbar v(Qt::Vertical);
  v.attach(&area);

  const int w = area.viewport()->width();
  const int ht = area.viewport()->height();
  const QPointF corner(w - 3, ht - 3);
  QMouseEvent move(
      QEvent::MouseMove, corner, area.viewport()->mapToGlobal(corner), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(area.viewport(), &move);

  EXPECT_TRUE(h.isShown()) << "the bottom-right corner reveals the horizontal pill";
  EXPECT_FALSE(v.isShown()) << "the vertical pill yields the bottom-right corner";
}

TEST(ScrollbarWidget, AttachHidesNativeAndTracksRange) {
  QScrollArea area;
  area.setWidget(new QWidget);
  area.widget()->setFixedSize(2000, 2000);
  area.resize(200, 200);
  area.show();

  PJ::Scrollbar bar(Qt::Vertical);
  bar.attach(&area);

  EXPECT_EQ(area.verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);

  // Scroll to the bottom so the pill should be positioned toward the end.
  QScrollBar* native = area.verticalScrollBar();
  native->setValue(native->maximum());

  // computeHandle with the live bar state must produce a non-empty handle
  // positioned away from the origin — confirming the overlay tracks the bar.
  const double track_len = static_cast<double>(area.viewport()->height());
  const Handle h = computeHandle(native->minimum(), native->maximum(), native->value(), native->pageStep(), track_len);

  EXPECT_GT(h.len, 0.0) << "handle must be non-empty when there is content to scroll";
  EXPECT_GT(h.pos, 0.0) << "handle must be positioned toward the end after scrolling to max";
}
