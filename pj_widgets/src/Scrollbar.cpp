// SPDX-License-Identifier: MPL-2.0
#include "pj_widgets/Scrollbar.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QChildEvent>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ::scrollbar_detail {

Handle computeHandle(long min, long max, long value, long page_step, double track_len_px) {
  // page_step <= 0 is a degenerate scroll state (e.g. a momentarily collapsed /
  // zero-extent viewport): there is no real draggable handle, so paint nothing
  // rather than flooring a phantom pill to kMinPillLenPx.
  if (max <= min || page_step <= 0 || track_len_px <= 0.0) {
    return {0.0, 0.0};
  }
  const double total = static_cast<double>(max - min) + static_cast<double>(page_step);
  if (total <= 0.0) {
    return {0.0, 0.0};
  }
  double len = std::max(kMinPillLenPx, (static_cast<double>(page_step) / total) * track_len_px);
  len = std::min(len, track_len_px);
  const double frac = (static_cast<double>(value - min)) / total;
  double pos = std::clamp(frac * track_len_px, 0.0, track_len_px - len);
  return {pos, len};
}

long valueForDrag(long start_value, double delta_px, double track_len_px, long total) {
  if (track_len_px <= 0.0) {
    return start_value;
  }
  return start_value + static_cast<long>(std::llround((delta_px / track_len_px) * static_cast<double>(total)));
}

QColor defaultAccent(const QColor& window_color) {
  // The app sets QPalette::Window per theme but not QPalette::Highlight, so
  // derive the theme from window lightness and read the framework scroll handle.
  return theme::surface(theme::Surface::ScrollHandle, theme::themeFor(window_color.lightness() >= 128));
}

}  // namespace PJ::scrollbar_detail

namespace PJ {

Scrollbar::Scrollbar(Qt::Orientation orientation, QWidget* parent) : QWidget(parent), orientation_(orientation) {
  // Transparent overlay: never intercepts mouse events, paints over the native
  // scroll-bar gutter without a system background or opaque window behind it.
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_TranslucentBackground, true);

  // Theme-aware default accent (info-blue on light, pale grey on dark), keyed
  // off QPalette::Window — NOT QPalette::Highlight, which the app never sets and
  // would otherwise leak the OS accent (e.g. Yaru orange). Re-read on theme
  // changes in changeEvent() unless the caller pins a color via setAccentColor().
  accent_ = scrollbar_detail::defaultAccent(QGuiApplication::palette().window().color());
}

void Scrollbar::attach(QAbstractScrollArea* area) {
  area_ = area;
  viewport_ = area->viewport();
  // Stamp the marker EVERY attach path sets, so a host that attaches its own
  // pills directly (e.g. the Timeline) is skipped by the bulk adaptScrollAreas
  // helper instead of receiving a second, unmanaged pair.
  area->setProperty("pjScrollbarAttached", true);

  // Reparent so this widget lives inside the scroll area's coordinate space.
  setParent(area);

  // Hide the native bar's visual widget; leave the bar object live because it
  // remains the single source of truth for scroll state.
  if (orientation_ == Qt::Horizontal) {
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  } else {
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  }

  // Reposition + repaint whenever the native bar moves or its range changes
  // (range changes also fire on viewport resize, since pageStep changes).
  QScrollBar* b = bar();
  connect(b, &QScrollBar::valueChanged, this, &Scrollbar::recomputeGeometry);
  connect(b, &QScrollBar::rangeChanged, this, [this](int, int) { recomputeGeometry(); });

  // Opacity effect starting fully hidden; fades in/out on hover.
  opacity_effect_ = new QGraphicsOpacityEffect(this);
  opacity_effect_->setOpacity(0.0);
  setGraphicsEffect(opacity_effect_);

  fade_anim_ = new QPropertyAnimation(opacity_effect_, "opacity", this);
  fade_anim_->setDuration(fade_ms_);

  // Single-shot timer that fades the pill back out after a scroll-driven reveal.
  // On timeout, hand control to the hover path: keep the pill only if the cursor
  // is now over the strip, otherwise hide it. An active drag owns visibility, so
  // the timer never hides mid-drag.
  hide_timer_ = new QTimer(this);
  hide_timer_->setSingleShot(true);
  connect(hide_timer_, &QTimer::timeout, this, [this]() {
    if (dragging_ || viewport_.isNull()) {
      return;
    }
    const QPoint vp_pos = viewport_->mapFromGlobal(QCursor::pos());
    setShown(pointInStrip(vp_pos));
  });

  // Observe the viewport for hover + resize events (we're transparent to mouse
  // so the viewport receives all pointer events underneath us).
  //
  // Enable mouse tracking so the viewport delivers MouseMove events even when no
  // button is held — otherwise Qt only sends moves during a drag, the hover
  // show/hide path never runs, and the pill only ever appears on click (and then
  // fails to re-hide on intra-viewport motion). The Timeline already enables this
  // for its own item-hover cursors, but a generic adapted scroll area does not,
  // so attach() must do it to be self-sufficient.
  area->viewport()->setMouseTracking(true);
  area->viewport()->installEventFilter(this);

  // When child widgets fully cover the viewport (e.g. a QScrollArea of cards),
  // hover MouseMoves are delivered to those children, not the viewport, so the
  // viewport filter above never sees them and the pill would only reveal on
  // scroll/click. Observe the covering subtree too (future cards via ChildAdded).
  for (QObject* child : area->viewport()->children()) {
    if (child->isWidgetType()) {
      installHoverObserver(static_cast<QWidget*>(child));
    }
  }

  // Make visible and ensure we paint on top of other children.
  // Opacity starts at 0, so "visible" means "ready to be faded in on hover".
  show();
  raise();

  recomputeGeometry();

  // FIX 4: honor setAutoHide(false) called before attach(). attach() just
  // created opacity_effect_ at opacity 0; if the caller already opted out of
  // auto-hide we must immediately force visible, mirroring what setAutoHide()
  // does when called post-attach.
  if (!auto_hide_) {
    if (fade_anim_ != nullptr) {
      fade_anim_->stop();
    }
    opacity_effect_->setOpacity(1.0);
    shown_ = true;
  }
}

void Scrollbar::setAccentColor(const QColor& color) {
  accent_ = color;
  accent_overridden_ = true;
  update();
}

void Scrollbar::setAutoHide(bool auto_hide) {
  auto_hide_ = auto_hide;
  if (!auto_hide && opacity_effect_) {
    // Force fully visible immediately — no animation.
    fade_anim_->stop();
    opacity_effect_->setOpacity(1.0);
    shown_ = true;
  }
  // Re-enabling auto-hide: the next MouseMove/Leave from the viewport drives
  // the correct state, so no immediate action is needed.
}

void Scrollbar::setFadeDurationMs(int ms) {
  fade_ms_ = ms;
  if (fade_anim_) {
    fade_anim_->setDuration(ms);
  }
}

void Scrollbar::setClickToScroll(bool enabled) {
  click_to_scroll_ = enabled;
}

void Scrollbar::setInteractive(bool interactive) {
  interactive_ = interactive;
  if (!interactive) {
    // Lock engaged: cancel any active drag and pending reveal hold, and retreat
    // the pill. Otherwise a pill grabbed (or scroll-revealed) at the instant the
    // lock engages would stay stuck fully visible over the frozen view, with no
    // event scheduled to hide it. Hover still re-reveals it on the next move.
    const bool changed = dragging_ && area_ != nullptr && bar()->value() != drag_origin_value_;
    dragging_ = false;
    if (hide_timer_ != nullptr) {
      hide_timer_->stop();
    }
    setShown(false);
    if (changed) {
      emit scrollChangeCommitted();
    }
  }
}

void Scrollbar::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (!accent_overridden_ &&
      (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
       event->type() == QEvent::StyleChange)) {
    accent_ = scrollbar_detail::defaultAccent(QGuiApplication::palette().window().color());
    update();
  }
}

double Scrollbar::currentOpacity() const {
  return opacity_effect_ ? opacity_effect_->opacity() : 1.0;
}

void Scrollbar::setShown(bool shown) {
  if (!auto_hide_) {
    return;
  }
  // Suppress show when there is nothing to scroll (mirrors ScrollBarAsNeeded).
  const bool want = shown && (bar()->maximum() > bar()->minimum());
  if (want == shown_) {
    return;
  }
  shown_ = want;
  if (want) {
    raise();
  }
  fade_anim_->stop();
  if (fade_ms_ == 0) {
    // Instant snap: bypass the animation entirely so opacity is set synchronously
    // in this call frame (QPropertyAnimation::start() with duration=0 is deferred
    // to the next event-loop tick via the unified animation timer, which would make
    // "instant" invisible to callers that check opacity directly after the call).
    opacity_effect_->setOpacity(want ? 1.0 : 0.0);
  } else {
    fade_anim_->setStartValue(opacity_effect_->opacity());
    fade_anim_->setEndValue(want ? 1.0 : 0.0);
    fade_anim_->start();
  }
}

void Scrollbar::revealTemporarily() {
  if (!auto_hide_ || !interactive_) {
    // Pinned visible (nothing to time out), or locked — in the locked case a
    // scroll won't move the frozen view, so revealing the pill would be a
    // misleading affordance. Hover-reveal still works while locked (it goes
    // through setShown directly, not here).
    return;
  }
  setShown(true);  // internally suppressed when there's nothing to scroll
  if (shown_) {
    hide_timer_->start(scrollbar_detail::kScrollRevealHoldMs);
  }
}

bool Scrollbar::scrollRevealActive() const {
  return hide_timer_ != nullptr && hide_timer_->isActive();
}

bool Scrollbar::pointInStrip(const QPoint& viewport_pos) const {
  if (area_ == nullptr || viewport_.isNull()) {
    return false;
  }
  const QWidget* vp = viewport_;
  const int strip = static_cast<int>(scrollbar_detail::kHoverStripPx);
  if (orientation_ == Qt::Horizontal) {
    return viewport_pos.y() >= vp->height() - strip && viewport_pos.x() >= 0 && viewport_pos.x() < vp->width();
  } else {
    // Vertical strip down the right edge. Yield the bottom-right corner to the
    // horizontal pill (whose strip also claims it) so its right end stays
    // grabbable — but only when the horizontal axis actually has a handle there;
    // for a vertical-only area the full-height strip stays usable.
    int bottom = vp->height();
    const QScrollBar* hbar = area_->horizontalScrollBar();
    if (hbar->maximum() > hbar->minimum()) {
      bottom -= strip;
    }
    return viewport_pos.x() >= vp->width() - strip && viewport_pos.y() >= 0 && viewport_pos.y() < bottom;
  }
}

void Scrollbar::applyHoverAt(const QPoint& viewport_pos) {
  // Entering the strip shows the pill; leaving it hides — UNLESS a
  // scroll/interaction reveal is still holding it (a hover-out, or the synthetic
  // MouseMove Qt delivers as scrolled content slides under a stationary cursor,
  // must not cancel that reveal; the hold timer owns the eventual hide).
  if (pointInStrip(viewport_pos)) {
    setShown(true);
  } else if (!scrollRevealActive()) {
    setShown(false);
  }
}

void Scrollbar::installHoverObserver(QWidget* widget) {
  if (widget == nullptr) {
    return;
  }
  widget->setMouseTracking(true);
  widget->installEventFilter(this);
  for (QObject* child : widget->children()) {
    if (child->isWidgetType()) {
      installHoverObserver(static_cast<QWidget*>(child));
    }
  }
}

bool Scrollbar::eventFilter(QObject* watched, QEvent* event) {
  if (area_ == nullptr) {
    return false;
  }
  QWidget* viewport = area_->viewport();

  // A newly-added descendant anywhere in the observed subtree (a fresh card, or a
  // content widget set on the scroll area) must be observed for hover too, so the
  // pill keeps revealing as the content grows.
  if (event->type() == QEvent::ChildAdded) {
    auto* ce = static_cast<QChildEvent*>(event);
    if (ce->child() != nullptr && ce->child()->isWidgetType()) {
      installHoverObserver(qobject_cast<QWidget*>(ce->child()));
    }
    return false;
  }

  // Events from a covering child (see installHoverObserver). Hover MouseMoves
  // are mapped into viewport space to drive show/hide without consuming. The
  // one interaction we DO own from a child is a drag that starts on the
  // VISIBLE pill handle: without it, a child widget that overlaps the strip
  // (e.g. a list row's trailing action button) receives the press instead of
  // the pill the user is looking at — grabbing the handle could activate the
  // child. Presses anywhere else on the child (including the empty strip —
  // never click-to-scroll from a child) are left untouched.
  if (watched != viewport) {
    auto* w = qobject_cast<QWidget*>(watched);
    if (w == nullptr) {
      return false;
    }
    const auto mapped = [&](QMouseEvent* me) {
      return viewport->mapFromGlobal(w->mapToGlobal(me->position().toPoint()));
    };
    switch (event->type()) {
      case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(event);
        const QPoint pos = mapped(me);
        if (dragging_) {
          if (!interactive_) {
            dragging_ = false;
          } else {
            const double axis_px = (orientation_ == Qt::Horizontal) ? pos.x() : pos.y();
            const double track_len = (orientation_ == Qt::Horizontal) ? static_cast<double>(viewport->width())
                                                                      : static_cast<double>(viewport->height());
            const long total = (bar()->maximum() - bar()->minimum()) + bar()->pageStep();
            bar()->setValue(
                static_cast<int>(scrollbar_detail::valueForDrag(
                    drag_start_value_, axis_px - drag_start_axis_px_, track_len, total)));
            return true;
          }
        }
        applyHoverAt(pos);
        return false;
      }
      case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() != Qt::LeftButton || !interactive_ || !shown_) {
          return false;
        }
        const QPoint pos = mapped(me);
        if (!pointInStrip(pos) || bar()->maximum() <= bar()->minimum()) {
          return false;
        }
        const double axis_px = (orientation_ == Qt::Horizontal) ? pos.x() : pos.y();
        const bool on_handle = (axis_px >= handle_pos_) && (axis_px <= handle_pos_ + handle_len_);
        if (!on_handle) {
          return false;
        }
        drag_origin_value_ = bar()->value();
        drag_start_value_ = bar()->value();
        drag_start_axis_px_ = axis_px;
        dragging_ = true;
        setShown(true);
        return true;  // consume: the press belongs to the pill, not the child
      }
      case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (!dragging_ || me->button() != Qt::LeftButton) {
          return false;
        }
        const bool changed = bar()->value() != drag_origin_value_;
        dragging_ = false;
        if (pointInStrip(mapped(me))) {
          setShown(true);
        } else {
          revealTemporarily();
        }
        if (changed && interactive_) {
          emit scrollChangeCommitted();
        }
        return true;  // consume: paired with the press we consumed
      }
      default:
        return false;
    }
  }

  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      auto* me = static_cast<QMouseEvent*>(event);
      if (me->button() != Qt::LeftButton) {
        break;
      }
      // FIX 1: when not interactive, ignore the press — do not start a drag and
      // do not consume the event so the Timeline's own filter handles it.
      if (!interactive_) {
        break;
      }
      const QPoint pos = me->position().toPoint();
      if (!pointInStrip(pos) || bar()->maximum() <= bar()->minimum()) {
        break;
      }

      const double axis_px = (orientation_ == Qt::Horizontal) ? pos.x() : pos.y();
      const bool on_handle = (axis_px >= handle_pos_) && (axis_px <= handle_pos_ + handle_len_);
      drag_origin_value_ = bar()->value();

      // By default only the visible handle is a drag target: a press on the empty
      // track (in the strip but off the handle) is NOT consumed, so it falls
      // through to the underlying view — otherwise the overlay would silently
      // swallow content clicks (item selection, etc.) along the whole edge strip,
      // where the native bar used to reserve a non-content gutter but now does not
      // (it is forced AlwaysOff and content reflows under the strip). A host that
      // owns its whole viewport (the Timeline) opts into click_to_scroll_, where an
      // off-handle press first centers the handle under the cursor, then drags.
      if (!on_handle) {
        if (!click_to_scroll_) {
          break;
        }
        const double track_len = (orientation_ == Qt::Horizontal) ? static_cast<double>(viewport_->width())
                                                                  : static_cast<double>(viewport_->height());
        const long total = (bar()->maximum() - bar()->minimum()) + bar()->pageStep();
        if (track_len > 0.0) {
          const double want_pos = std::clamp(axis_px - handle_len_ / 2.0, 0.0, std::max(0.0, track_len - handle_len_));
          bar()->setValue(
              bar()->minimum() + static_cast<int>(std::llround((want_pos / track_len) * static_cast<double>(total))));
        }
      }

      // Capture drag origin AFTER any click-to-center value change.
      drag_start_value_ = bar()->value();
      drag_start_axis_px_ = axis_px;
      dragging_ = true;
      setShown(true);
      return true;  // consume: prevent viewport row-selection / pan on press
    }

    case QEvent::MouseMove: {
      auto* me = static_cast<QMouseEvent*>(event);
      const QPoint pos = me->position().toPoint();

      if (dragging_) {
        // FIX 1: if a drag was in progress when the lock engaged, abandon it
        // without consuming — fall through to normal hover show/hide below.
        if (!interactive_) {
          dragging_ = false;
        } else {
          const double axis_px = (orientation_ == Qt::Horizontal) ? pos.x() : pos.y();
          const QWidget* vp = area_->viewport();
          const double track_len =
              (orientation_ == Qt::Horizontal) ? static_cast<double>(vp->width()) : static_cast<double>(vp->height());
          const long total = (bar()->maximum() - bar()->minimum()) + bar()->pageStep();
          bar()->setValue(
              static_cast<int>(
                  scrollbar_detail::valueForDrag(drag_start_value_, axis_px - drag_start_axis_px_, track_len, total)));
          return true;  // consume: prevent viewport pan/selection during drag
        }
      }

      // Not dragging (or drag just cleared): normal hover show/hide.
      applyHoverAt(pos);
      return false;
    }

    case QEvent::MouseButtonRelease: {
      if (dragging_) {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
          // FIX 1: if lock engaged mid-drag, clear drag state but do not consume.
          if (!interactive_) {
            dragging_ = false;
            break;
          }
          const bool changed = bar()->value() != drag_origin_value_;
          dragging_ = false;
          // Re-evaluate pill visibility from the release position. If the release
          // landed off the strip (easy to do — the cursor drifts off the 14px band
          // while dragging), don't vanish instantly; give it a grace hold so it
          // lingers, then hands back to the hover path.
          if (pointInStrip(me->position().toPoint())) {
            setShown(true);
          } else {
            revealTemporarily();
          }
          if (changed) {
            emit scrollChangeCommitted();
          }
          return true;  // consume: paired with the press we consumed
        }
      }
      break;
    }

    case QEvent::Wheel: {
      // Reveal the pill on a scroll, even when the cursor is nowhere near the
      // strip. Only the axis that actually scrolled reveals: angleDelta carries
      // the standard wheel/gesture delta, pixelDelta the high-resolution trackpad
      // variant; y drives the vertical bar, x the horizontal. Shift+vertical
      // wheel is the common "scroll horizontally" convention, so the horizontal
      // bar also honors a y-delta when Shift is held. Never consume — the scroll
      // area must still scroll.
      auto* we = static_cast<QWheelEvent*>(event);
      const QPoint angle = we->angleDelta();
      const QPoint pixels = we->pixelDelta();
      int axis_delta = 0;
      if (orientation_ == Qt::Horizontal) {
        axis_delta = (angle.x() != 0) ? angle.x() : pixels.x();
        if (axis_delta == 0 && (we->modifiers() & Qt::ShiftModifier)) {
          axis_delta = (angle.y() != 0) ? angle.y() : pixels.y();
        }
      } else {
        axis_delta = (angle.y() != 0) ? angle.y() : pixels.y();
      }
      if (axis_delta != 0) {
        revealTemporarily();
      }
      break;
    }

    case QEvent::Leave:
      // Don't hide while a drag is active — the cursor has left the viewport strip
      // but the drag is still in progress (e.g. a fast flick took the pointer
      // outside the viewport). Likewise don't hide while a scroll/interaction
      // reveal is still holding the pill; the hold timer owns that hide.
      if (!dragging_ && !scrollRevealActive()) {
        setShown(false);
      }
      break;

    case QEvent::Resize:
      recomputeGeometry();
      break;

    default:
      break;
  }

  return false;
}

QScrollBar* Scrollbar::bar() const {
  return (orientation_ == Qt::Horizontal) ? area_->horizontalScrollBar() : area_->verticalScrollBar();
}

void Scrollbar::recomputeGeometry() {
  // viewport_ may have been destroyed before this overlay during host teardown,
  // while the still-live bar fires one last valueChanged/rangeChanged at us.
  if (area_ == nullptr || viewport_.isNull()) {
    return;
  }
  const QWidget* vp = viewport_;
  const int strip = static_cast<int>(scrollbar_detail::kHoverStripPx);

  // Position the overlay over the edge strip of the viewport, mapped into the
  // scroll area's coordinate space (our parent's coords after reparenting).
  QPoint tl;
  int w = 0;
  int h = 0;
  double track_len = 0.0;
  if (orientation_ == Qt::Horizontal) {
    // Bottom strip across the full viewport width.
    tl = vp->mapTo(area_, QPoint(0, vp->height() - strip));
    w = vp->width();
    h = strip;
    track_len = static_cast<double>(vp->width());
  } else {
    // Right strip down the full viewport height.
    tl = vp->mapTo(area_, QPoint(vp->width() - strip, 0));
    w = strip;
    h = vp->height();
    track_len = static_cast<double>(vp->height());
  }
  setGeometry(tl.x(), tl.y(), w, h);

  const QScrollBar* b = bar();
  const auto handle = scrollbar_detail::computeHandle(b->minimum(), b->maximum(), b->value(), b->pageStep(), track_len);
  handle_pos_ = handle.pos;
  handle_len_ = handle.len;

  update();
}

void Scrollbar::paintEvent(QPaintEvent* /*event*/) {
  if (handle_len_ <= 0.0) {
    return;
  }
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Center the pill across the strip's cross-axis; lay it along the scroll
  // axis. Port of TimelineScrollPill::paintEvent (Timeline.cpp).
  const double thickness = scrollbar_detail::kPillThicknessPx;
  QRectF r;
  if (orientation_ == Qt::Horizontal) {
    const double y = (static_cast<double>(height()) - thickness) / 2.0;
    r = QRectF(handle_pos_, y, handle_len_, thickness);
  } else {
    const double x = (static_cast<double>(width()) - thickness) / 2.0;
    r = QRectF(x, handle_pos_, thickness, handle_len_);
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(accent_);
  painter.drawRoundedRect(r, thickness / 2.0, thickness / 2.0);
}

void attachPillScrollbars(QWidget* root, const std::function<bool(QAbstractScrollArea*)>& skip) {
  if (root == nullptr) {
    return;
  }
  constexpr bool kDefaultAutoHide = true;
  constexpr int kDefaultFadeMs = 150;

  const QList<QAbstractScrollArea*> areas = root->findChildren<QAbstractScrollArea*>();
  for (QAbstractScrollArea* area : areas) {
    if (area->property("pjScrollbarAttached").toBool()) {
      continue;
    }
    if (skip && skip(area)) {
      continue;
    }
    // A combo-box's internal view / a transient popup item view (e.g. the list a
    // combo opens): a pill over a drop-down dismissed on selection would flicker.
    if (qobject_cast<QComboBox*>(area->parentWidget()) != nullptr) {
      continue;
    }
    if (qobject_cast<QAbstractItemView*>(area) != nullptr && (area->window()->windowFlags() & Qt::Popup) == Qt::Popup) {
      continue;
    }
    // An AlwaysOn axis is a deliberate persistent, draggable native bar (e.g. a
    // log view) that a hover-only pill would silently replace — leave it be.
    const bool adapt_h = area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOn;
    const bool adapt_v = area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOn;
    if (!adapt_h && !adapt_v) {
      continue;
    }
    const bool auto_hide = area->property("pjScrollbarAutoHide").isValid()
                               ? area->property("pjScrollbarAutoHide").toBool()
                               : kDefaultAutoHide;
    const int fade_ms =
        area->property("pjScrollbarFadeMs").isValid() ? area->property("pjScrollbarFadeMs").toInt() : kDefaultFadeMs;
    const auto attach_pill = [&](Qt::Orientation orientation) {
      auto* pill = new Scrollbar(orientation, area);
      pill->attach(area);
      pill->setAutoHide(auto_hide);
      pill->setFadeDurationMs(fade_ms);
    };
    if (adapt_h) {
      attach_pill(Qt::Horizontal);
    }
    if (adapt_v) {
      attach_pill(Qt::Vertical);
    }
    area->setProperty("pjScrollbarAttached", true);
  }
}

}  // namespace PJ
