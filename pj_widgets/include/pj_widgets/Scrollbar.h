// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QPointer>
#include <QWidget>
#include <functional>

#include "pj_widgets/FrameworkTokens.h"

class QAbstractScrollArea;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QScrollBar;
class QTimer;

namespace PJ::scrollbar_detail {

inline constexpr double kMinPillLenPx = 28.0;
inline constexpr double kPillThicknessPx = 6.0;
/// Width of the edge strip (in px) that the overlay widget occupies.
inline constexpr double kHoverStripPx = 14.0;
/// How long (ms) the pill stays revealed after the last scroll (wheel/gesture)
/// event before it fades back out — unless the cursor is then hovering the edge
/// strip, in which case the normal hover path keeps it shown.
inline constexpr int kScrollRevealHoldMs = 900;

/// Pick the default pill accent for the theme identified by `window_color`
/// (QPalette::Window). Pure helper so the choice is unit-testable without a
/// live widget/palette.
[[nodiscard]] QColor defaultAccent(const QColor& window_color);

/// Handle geometry along the scroll axis: `pos` is the start offset in px
/// from the track origin, `len` is the pill length in px. Both are zero when
/// there is nothing to scroll.
struct Handle {
  double pos = 0.0;
  double len = 0.0;
};

/// Map scroll state to a pill handle along a track of `track_len_px`.
/// Returns {0,0} when there is nothing to scroll (max <= min).
[[nodiscard]] Handle computeHandle(long min, long max, long value, long page_step, double track_len_px);

/// Map a drag delta in viewport pixels to a native scroll bar value.
/// Returns `start_value + llround((delta_px / track_len_px) * total)` where
/// `total = (bar->maximum() - bar->minimum()) + bar->pageStep()`.
/// Guard: returns `start_value` unchanged when `track_len_px <= 0`.
[[nodiscard]] long valueForDrag(long start_value, double delta_px, double track_len_px, long total);

}  // namespace PJ::scrollbar_detail

namespace PJ {

/// Overlay scroll-pill widget that attaches to a QAbstractScrollArea and
/// mirrors its native scroll bar as a thin floating pill.
///
/// After attach():
///   - The matching native bar policy is forced to ScrollBarAlwaysOff (the bar
///     remains live as the scroll-state source of truth; only its visual widget
///     is hidden).
///   - This widget re-parents itself to the area, covers the edge strip of the
///     viewport, and repaints whenever the native bar's value or range changes.
///   - A QGraphicsOpacityEffect fades the pill in/out; the pill is hidden
///     (opacity 0) by default and revealed when the cursor enters the edge strip
///     (auto-hide mode). Call setAutoHide(false) to keep the pill always visible.
///   - Scrolling the viewport (mouse wheel or trackpad scroll gesture) also
///     briefly reveals the matching-axis pill even when the cursor is nowhere
///     near the strip; it fades out kScrollRevealHoldMs after the last scroll
///     unless the cursor is then hovering the strip.
///
/// Drag-to-scroll: a left-press anywhere in the edge strip grabs the pill.
/// Pressing off the handle first centers the handle under the cursor, then
/// tracks the drag delta. Events during an active drag are consumed (return
/// true) so the underlying view is not affected. Hover events (no drag) are
/// still observed-only (return false).
class Scrollbar : public QWidget {
  Q_OBJECT

 public:
  explicit Scrollbar(Qt::Orientation orientation, QWidget* parent = nullptr);

  /// Attach to a scroll area. Safe to call only once; re-attaching is not
  /// supported. After this call the widget covers the viewport's edge strip
  /// and fades in on hover (auto-hide on by default).
  /// Side-effect: enables mouse tracking on the area's viewport so hover
  /// (button-up) MouseMove events are delivered — without it the pill would
  /// only appear on click and never re-hide on intra-viewport motion.
  void attach(QAbstractScrollArea* area);

  /// Override the accent color used for the pill. By default the pill picks a
  /// theme-aware accent via scrollbar_detail::defaultAccent() (info-blue on a
  /// light theme, pale grey on a dark one), re-evaluated on theme changes; call
  /// this only when an explicit per-instance color is needed (it pins the color
  /// and opts out of the automatic theme tracking).
  void setAccentColor(const QColor& color);

  /// Enable or disable auto-hide behaviour (default: true).
  /// When false the pill is immediately forced to full opacity and stays
  /// visible regardless of cursor position. When true the normal hover/fade
  /// behaviour resumes (opacity is not changed immediately — the next
  /// MouseMove/Leave event on the viewport will drive it).
  void setAutoHide(bool auto_hide);

  /// Set the fade-in/out animation duration in milliseconds (default: 150).
  /// Pass 0 for an instant snap to the target opacity.
  /// Safe to call before or after attach().
  void setFadeDurationMs(int ms);

  /// Return the current rendered opacity [0.0, 1.0]. Primarily a test hook.
  double currentOpacity() const;

  /// Return whether the pill is logically shown (the fade animation end target).
  /// True immediately after setShown(true) regardless of animation progress.
  /// Primarily a test hook; matches the behaviour of the old pill_shown_ flag.
  bool isShown() const {
    return shown_;
  }

  /// Return the current fade animation duration in ms. Primarily a test hook.
  int fadeDurationMs() const {
    return fade_ms_;
  }

  /// Enable or disable pill-drag and scroll-on-drag interaction (default: true).
  /// When false the event filter still observes hover for show/hide but will not
  /// start or continue a drag, and will not consume any mouse events.
  /// Any drag active at the time of disable is immediately cancelled.
  /// Used by Timeline::setInteractionLocked to prevent scroll-pill drag when the
  /// timeline is locked (the Scrollbar filter runs first in the LIFO chain, so
  /// it must be disabled explicitly — returning early in mousePressEvent is not
  /// sufficient).
  void setInteractive(bool interactive);
  [[nodiscard]] bool isInteractive() const {
    return interactive_;
  }

  /// Enable click-to-scroll on the empty track (default: false).
  /// When false only the visible handle is a drag target, so a press on the empty
  /// edge strip falls through to the underlying view and never steals a content
  /// click — the safe default for a pill overlaid on arbitrary content (e.g.
  /// adaptScrollAreas over plugin item views). When true a press anywhere in the
  /// strip grabs, first centering the handle under an off-handle press; use it
  /// only when the host owns the whole viewport (the Timeline).
  void setClickToScroll(bool enabled);
  [[nodiscard]] bool clickToScroll() const {
    return click_to_scroll_;
  }

 signals:
  /// A user pill drag ended after changing the backing scrollbar.
  void scrollChangeCommitted();

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  /// Re-read the accent color from the application palette on palette/style
  /// changes, unless the caller explicitly overrode it via setAccentColor().
  void changeEvent(QEvent* event) override;

 private:
  /// Recompute this widget's geometry (edge-strip rect, mapped into the
  /// parent area's coordinate space) and the handle position/length from the
  /// live native bar. Calls update().
  void recomputeGeometry();

  /// Return the area's native scroll bar matching orientation_.
  /// Undefined behavior if area_ is null.
  QScrollBar* bar() const;

  /// Animate the pill to shown (opacity 1) or hidden (opacity 0).
  /// Ignored when auto_hide_ is false, and suppresses show when the bar has
  /// nothing to scroll (maximum <= minimum), matching ScrollBarAsNeeded semantics.
  void setShown(bool shown);

  /// Reveal the pill and (re)arm the auto-hide hold timer. Used both for a scroll
  /// (wheel/gesture) and as a grace period after a drag release that lands off
  /// the strip, so the pill lingers briefly instead of vanishing instantly.
  /// No-op when auto-hide is off (the pill is already pinned visible) or when
  /// there is nothing to scroll. When the hold elapses, hide_timer_ hands control
  /// back to the hover path (stay shown iff the cursor is over the strip).
  void revealTemporarily();

  /// True while a scroll/interaction reveal is still holding the pill visible
  /// (hide_timer_ armed). During this window the hover path must not *hide* the
  /// pill — a hover-out, or a synthetic MouseMove Qt delivers as scrolled content
  /// slides under a stationary cursor, would otherwise cancel the reveal too
  /// eagerly. The hold timer owns the eventual hide.
  [[nodiscard]] bool scrollRevealActive() const;

  /// Return true when viewport_pos (in viewport coordinates) is inside the
  /// edge strip for this orientation (the kHoverStripPx-wide band where the
  /// pill lives).
  bool pointInStrip(const QPoint& viewport_pos) const;

  /// Drive the hover show/hide from a position already mapped into viewport
  /// coordinates: reveal when inside the strip, hide when outside (unless a
  /// scroll/interaction reveal is still holding the pill). Shared by the
  /// viewport filter and the covering-child observers.
  void applyHoverAt(const QPoint& viewport_pos);

  /// Recursively enable mouse tracking on `widget` and install this overlay as
  /// its event filter, so hover MouseMoves reach applyHoverAt() even when child
  /// widgets fully cover the viewport (e.g. a QScrollArea packed with cards);
  /// without it the pill would only ever reveal on scroll/click. New children
  /// are picked up via QEvent::ChildAdded in eventFilter(). Observe-only — these
  /// events are never consumed, so the widgets' own interaction is untouched.
  void installHoverObserver(QWidget* widget);

  Qt::Orientation orientation_;
  QAbstractScrollArea* area_ = nullptr;
  // Cached viewport, held as a QPointer so it auto-nulls if the viewport is
  // destroyed before this overlay. Guards recomputeGeometry() (wired to the live
  // bar's valueChanged/rangeChanged) against a dangling deref during host
  // teardown, where the area's dtor can still emit a range/value change while the
  // viewport subobject is already gone (the hazard ~Timeline disconnects to avoid;
  // here the pill is parented to the area and outlives that window).
  QPointer<QWidget> viewport_;
  QColor accent_;
  double handle_pos_ = 0.0;
  double handle_len_ = 0.0;

  QGraphicsOpacityEffect* opacity_effect_ = nullptr;
  QPropertyAnimation* fade_anim_ = nullptr;
  // Single-shot timer that fades the pill out kScrollRevealHoldMs after the last
  // scroll/interaction reveal (see revealTemporarily()). Restarted on each one.
  QTimer* hide_timer_ = nullptr;
  bool shown_ = false;
  bool auto_hide_ = true;
  int fade_ms_ = 150;

  // Drag-to-scroll state (Task 4). Set on MouseButtonPress in the strip;
  // cleared on MouseButtonRelease. drag_start_value_ is bar()->value() at the
  // moment the drag was captured (after any click-to-center jump).
  // drag_start_axis_px_ is the viewport coordinate along the scroll axis at
  // the capture point.
  bool dragging_ = false;
  // When true, a press anywhere in the strip grabs (off-handle presses jump the
  // handle under the cursor first); when false only the handle is a drag target.
  // Opt-in (Timeline), so an overlay on foreign content never steals strip clicks.
  bool click_to_scroll_ = false;
  long drag_origin_value_ = 0;
  long drag_start_value_ = 0;
  double drag_start_axis_px_ = 0.0;

  // Interaction lock: when false the filter observes hover for show/hide but
  // never starts/continues a drag and never consumes mouse events. Set by
  // setInteractive(); cleared drags immediately on disable.
  bool interactive_ = true;
  // Whether accent_ was explicitly set via setAccentColor(). When false the
  // color is re-read from the application palette on each changeEvent so the
  // pill follows theme changes automatically.
  bool accent_overridden_ = false;
};

/// Attach canonical overlay pill scrollbars (PJ::Scrollbar) to every
/// QAbstractScrollArea under `root`, in place of their native bars — the one
/// call that gives an app window/dialog the same scroll pills the Timeline and
/// plugin dialogs already use. Idempotent: each adapted area is tagged with a
/// "pjScrollbarAttached" dynamic property, so re-calling after new views appear
/// only pills the newcomers.
///
/// Per area it honours the shared conventions:
///   - skips a combo-box's internal view and any transient popup item view;
///   - skips an axis pinned to Qt::ScrollBarAlwaysOn (that axis keeps its
///     draggable native bar); ScrollBarAsNeeded/AlwaysOff get a pill;
///   - reads optional per-area "pjScrollbarAutoHide" (bool) / "pjScrollbarFadeMs"
///     (int) dynamic properties to override the overlay defaults.
///
/// `skip`, when set, is an extra per-area veto (return true to leave an area's
/// native bars untouched) — e.g. the dialog host vetoes its composite widgets'
/// internal scroll areas.
void attachPillScrollbars(QWidget* root, const std::function<bool(QAbstractScrollArea*)>& skip = {});

}  // namespace PJ
