// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Timeline.h"

#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsOpacityEffect>
#include <QGraphicsRectItem>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScrollBar>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include "pj_widgets/SvgUtil.h"
#include "pj_widgets/ThemeColors.h"

namespace PJ {

// =============================================================================
// TimelineScene — Qt-free timing/geometry/alignment math.
// =============================================================================

namespace {
constexpr double kMinPxPerNs = 1e-12;
constexpr double kMaxPxPerNs = 1e-1;
constexpr int kMaxTicks = 10000;  // runaway guard

// Human-friendly tick intervals in ns: 100 us / 200 us / 500 us / 1/2/5/10/20/50 ms ... up to 1 h.
constexpr std::array<qint64, 23> kTickLadder = {
    100'000LL,         200'000LL,         500'000LL,         1'000'000LL,         2'000'000LL,        5'000'000LL,
    10'000'000LL,      20'000'000LL,      50'000'000LL,      100'000'000LL,       200'000'000LL,      500'000'000LL,
    1'000'000'000LL,   2'000'000'000LL,   5'000'000'000LL,   10'000'000'000LL,    30'000'000'000LL,   60'000'000'000LL,
    120'000'000'000LL, 300'000'000'000LL, 600'000'000'000LL, 1'800'000'000'000LL, 3'600'000'000'000LL};

qint64 chooseInterval(double px_per_ns, double target_px_per_tick) noexcept {
  const auto want = static_cast<qint64>(std::llround(target_px_per_tick / px_per_ns));
  for (const qint64 candidate : kTickLadder) {
    if (candidate >= want) {
      return candidate;
    }
  }
  return kTickLadder.back();
}

// Smallest multiple of `interval` that is >= value (handles negatives exactly,
// no double precision loss at epoch scale).
qint64 ceilToMultiple(qint64 value, qint64 interval) noexcept {
  const qint64 q = value / interval;  // truncates toward zero
  const qint64 floored = q * interval;
  if (floored == value) {
    return value;
  }
  return (value > 0) ? floored + interval : floored;
}
}  // namespace

void TimelineScene::setTracks(std::vector<TimelineSpanInput> tracks) {
  tracks_ = std::move(tracks);
}

TimeSpan TimelineScene::displayWindow(const TimelineSpanInput& track) noexcept {
  return {track.t_min_ns - track.offset_ns, track.t_max_ns - track.offset_ns};
}

TimeSpan TimelineScene::sceneExtent() const noexcept {
  std::optional<qint64> lo;
  std::optional<qint64> hi;
  for (const TimelineSpanInput& track : tracks_) {
    const auto w = displayWindow(track);
    lo = lo ? std::min(*lo, w.min) : w.min;
    hi = hi ? std::max(*hi, w.max) : w.max;
  }
  if (!lo) {
    return default_extent_;  // no tracks: the host-set empty span (defaults to 60 s)
  }
  return {*lo, *hi};
}

void TimelineScene::setDefaultExtent(qint64 min_ns, qint64 max_ns) noexcept {
  if (max_ns > min_ns) {
    default_extent_ = {min_ns, max_ns};
  }
}

double TimelineScene::nsToPx(qint64 display_ns, const TimelineViewport& vp) noexcept {
  return static_cast<double>(display_ns - vp.origin_ns) * vp.px_per_ns;
}

qint64 TimelineScene::pxToNs(double x_px, const TimelineViewport& vp) noexcept {
  return vp.origin_ns + static_cast<qint64>(std::llround(x_px / vp.px_per_ns));
}

PxSpan TimelineScene::barSpan(const TimelineSpanInput& track, const TimelineViewport& vp) noexcept {
  const auto w = displayWindow(track);
  const double x = nsToPx(w.min, vp);
  const double width = static_cast<double>(w.max - w.min) * vp.px_per_ns;
  return {x, width};
}

qint64 TimelineScene::pxDeltaToNs(double dx_px, const TimelineViewport& vp) noexcept {
  return static_cast<qint64>(std::llround(dx_px / vp.px_per_ns));
}

TimelineViewport TimelineScene::zoom(const TimelineViewport& vp, double factor, double anchor_x_px) noexcept {
  const double new_pp = std::clamp(vp.px_per_ns * factor, kMinPxPerNs, kMaxPxPerNs);
  const qint64 ns_at_anchor = pxToNs(anchor_x_px, vp);
  TimelineViewport out;
  out.px_per_ns = new_pp;
  out.origin_ns = ns_at_anchor - static_cast<qint64>(std::llround(anchor_x_px / new_pp));
  return out;
}

TimelineRuler TimelineScene::ruler(const TimelineViewport& vp, double width_px, double target_px_per_tick) {
  TimelineRuler out;
  out.interval_ns = chooseInterval(vp.px_per_ns, target_px_per_tick);
  const qint64 lo = vp.origin_ns;
  const qint64 hi = vp.origin_ns + static_cast<qint64>(std::llround(width_px / vp.px_per_ns));
  // Degenerate-extent guard: when the visible span is so large that even the widest
  // ladder interval (1 h) would pack thousands of ticks into the viewport — e.g. a
  // wall-clock-epoch streaming source unioned with relative-time file datasets, a span
  // of ~20,000 days — widen the interval so ticks stay at least kMinTickSpacingPx
  // apart. Without this the labels overpaint into a black smear and every repaint pays
  // for ~kMaxTicks labels + gridlines (the streaming "lags hard"). Round ladder
  // intervals are abandoned here (the labels are non-round), but that only happens in
  // the already-degenerate regime; normal zoom levels keep the ladder interval.
  constexpr double kMinTickSpacingPx = 48.0;
  if (hi > lo && out.interval_ns > 0 && width_px > 0.0) {
    const auto max_ticks_for_width = std::max<qint64>(1, static_cast<qint64>(width_px / kMinTickSpacingPx));
    const qint64 min_interval = (hi - lo) / max_ticks_for_width + 1;
    out.interval_ns = std::max(out.interval_ns, min_interval);
  }
  int count = 0;
  for (qint64 t = ceilToMultiple(lo, out.interval_ns); t <= hi && count < kMaxTicks; t += out.interval_ns, ++count) {
    out.ticks_ns.push_back(t);
  }
  return out;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignStartsToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  qint64 target = displayWindow(tracks_.front()).min;
  for (const TimelineSpanInput& track : tracks_) {
    target = std::min(target, displayWindow(track).min);
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    result.emplace_back(track.id, track.t_min_ns - target);
  }
  return result;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignCentersToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  // Displayed center of a track, overflow-safe: window.min + (window.max − window.min) / 2.
  const auto displayed_center = [](const TimelineSpanInput& track) -> qint64 {
    const TimeSpan w = displayWindow(track);
    return w.min + ((w.max - w.min) / 2);
  };
  qint64 target = displayed_center(tracks_.front());
  for (const TimelineSpanInput& track : tracks_) {
    target = std::min(target, displayed_center(track));
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    // We need raw_center − new_offset == target, and raw_center == displayed_center + offset.
    const qint64 raw_center = displayed_center(track) + track.offset_ns;
    result.emplace_back(track.id, raw_center - target);
  }
  return result;
}

std::vector<std::pair<TimelineSourceId, qint64>> TimelineScene::alignEndsToCommonOrigin() const {
  std::vector<std::pair<TimelineSourceId, qint64>> result;
  if (tracks_.empty()) {
    return result;
  }
  qint64 target = displayWindow(tracks_.front()).max;
  for (const TimelineSpanInput& track : tracks_) {
    target = std::max(target, displayWindow(track).max);
  }
  result.reserve(tracks_.size());
  for (const TimelineSpanInput& track : tracks_) {
    result.emplace_back(track.id, track.t_max_ns - target);
  }
  return result;
}

TimelineScene::EdgeSnap TimelineScene::snapToEdges(
    const std::vector<qint64>& dragged_edges, const std::vector<qint64>& candidate_edges, qint64 raw_delta_ns,
    qint64 threshold_ns) {
  EdgeSnap result{false, raw_delta_ns, 0};
  qint64 best_dist = threshold_ns + 1;
  for (const qint64 edge0 : dragged_edges) {
    for (const qint64 candidate : candidate_edges) {
      const qint64 align_delta = candidate - edge0;  // delta that lands edge0 on candidate
      const qint64 dist = std::llabs(align_delta - raw_delta_ns);
      if (dist < best_dist) {
        best_dist = dist;
        result.snapped = true;
        result.delta_ns = align_delta;
        result.edge_ns = candidate;
      }
    }
  }
  return result;
}

// =============================================================================
// Graphics items — bar / ruler / playhead. Module-private (timeline_detail).
// =============================================================================

namespace timeline_detail {

namespace {
// Chrono-derived (no drift from the unit it measures); kSecondNs == 1e9.
constexpr qint64 kSecondNs = std::chrono::nanoseconds::period::den;
constexpr qint64 kMillisecondNs = kSecondNs / 1000;
constexpr qint64 kMinuteNs = 60 * kSecondNs;
constexpr qint64 kHourNs = 60 * kMinuteNs;

// Absolute Unix-timestamp label (seconds) for a display-ns value read as epoch
// ns — the SAME value the plot axis shows in absolute mode (not a wall-clock
// HH:mm:ss, which would be timezone-dependent and hide the absolute value). The
// value is ROUNDED to the nearest millisecond — matching the playback readout's
// QString::number('f', 3) rounding, NOT truncated (truncation read a ms low and
// made the timeline disagree with playback by 0.001 s). Integer math (with carry
// into seconds) keeps it exact at epoch scale. `fixed_ms` forces a 3-decimal
// fraction even on a whole second (the marker pills, so they match the playback
// readout character-for-character); the ruler leaves it off for clean tick labels.
inline QString formatAbsoluteSeconds(qint64 epoch_ns, bool fixed_ms = false) {
  const qint64 half_ms = kMillisecondNs / 2;
  const qint64 total_ms = (epoch_ns + (epoch_ns >= 0 ? half_ms : -half_ms)) / kMillisecondNs;
  const qint64 sec = total_ms / 1000;
  const qint64 ms = (total_ms < 0 ? -total_ms : total_ms) % 1000;
  if (ms == 0 && !fixed_ms) {
    return QString::number(sec);
  }
  return QStringLiteral("%1.%2").arg(sec).arg(ms, 3, 10, QChar('0'));
}

// Linear RGB blend a*(1-t) + b*t.
QColor blendColor(const QColor& a, const QColor& b, double t) {
  const auto mix = [t](float x, float y) { return static_cast<float>((x * (1.0 - t)) + (y * t)); };
  return QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()));
}

// Theme-derived timeline colours. The app syncs QPalette::Window / WindowText per
// theme (Theme::syncApplicationPalette) and QSS does NOT touch the palette, so a
// self-painting widget must read QGuiApplication::palette() — never its own
// QSS-clobbered palette. Everything else is a blend of bg<->text so it adapts:
// white bg + dark-grey hatch + dark numbers in light, the inverse in dark.
struct TimelineColors {
  QColor bg;            // rows + ruler background (QPalette::Window)
  QColor ruler_data;    // subtle tint over the data (OR) span
  QColor grid_line;     // faint full-height tick gridlines
  QColor hatch_line;    // diagonal hatch over the empty (non-data) span
  QColor text;          // frame numbers + bar labels — "like any other text"
  QColor ruler_border;  // ruler bottom separator
  QColor bar_border;    // bar outline
};

TimelineColors timelineColors() {
  const QPalette pal = QGuiApplication::palette();
  const QColor bg = pal.color(QPalette::Window);
  const QColor text = pal.color(QPalette::WindowText);
  return {
      .bg = bg,
      .ruler_data = blendColor(bg, text, 0.06),
      .grid_line = blendColor(bg, text, 0.16),
      .hatch_line = blendColor(bg, text, 0.30),
      .text = text,
      .ruler_border = blendColor(bg, text, 0.32),
      .bar_border = blendColor(bg, text, 0.55),
  };
}
}  // namespace

/// One source rendered as a draggable bar. Ported from the PJ3 prototype's
/// TopicItem and rebound: it no longer reads a model — the widget feeds it pixel
/// geometry (from TimelineScene::barSpan) plus the SourceId/label/color for
/// hit-testing and painting. The "ghost" dx is a live, view-only preview of an
/// in-flight horizontal drag; the authoritative offset is written by the host in
/// response to offsetChangeRequested, never by this item.
class TimelineBarItem : public QGraphicsRectItem {
 public:
  TimelineBarItem(TimelineSourceId id, QString label, const QColor& fill)
      : id_(id), label_(std::move(label)), fill_(fill) {
    setAcceptHoverEvents(true);
    setCursor(Qt::OpenHandCursor);
    setZValue(10);
  }

  [[nodiscard]] TimelineSourceId sourceId() const noexcept {
    return id_;
  }

  /// Current live drag preview offset in pixels (0 when not being dragged).
  [[nodiscard]] double ghostDx() const noexcept {
    return ghost_dx_px_;
  }

  /// Mark this bar as part of the current multi-selection (heavier accent border).
  void setHighlighted(bool on) {
    if (on == highlighted_) {
      return;
    }
    highlighted_ = on;
    update();
  }

  /// Preview a pending horizontal drag of `dx_px` pixels (does not commit).
  void setGhostDx(double dx_px) {
    // boundingRect() widens by |ghost_dx_px_|, so this is a geometry change: the
    // scene's spatial index must be told before the rect changes, or the old
    // (narrower) cached rect leaves repaint trails during a drag.
    prepareGeometryChange();
    ghost_dx_px_ = dx_px;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    // Widen by the ghost offset so a previewed drag isn't clipped to the old rect.
    const QRectF r = QGraphicsRectItem::boundingRect();
    const double extra = std::abs(ghost_dx_px_);
    return r.adjusted(-extra, 0, extra, 0);
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const TimelineColors col = timelineColors();
    const QRectF r = rect().translated(ghost_dx_px_, 0);

    QColor fill = fill_;
    // Dim the fill while a drag preview is active so the original position stays
    // legible underneath the moving ghost.
    fill.setAlphaF(std::abs(ghost_dx_px_) < 0.001 ? 0.8f : 0.5f);
    painter->setBrush(fill);

    // Selected/grouped bars get a slightly heavier blue accent border as a hint.
    QPen border(highlighted_ ? QColor(0x3d, 0x8b, 0xff) : col.bar_border);
    border.setWidth(highlighted_ ? 2 : 1);
    painter->setPen(border);
    painter->drawRect(r);

    if (r.width() > 8.0) {
      const QFontMetricsF fm(painter->font());
      const QString text = fm.elidedText(label_, Qt::ElideRight, r.width() - 6.0);
      painter->setPen(col.text);
      painter->drawText(r.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
  }

 private:
  TimelineSourceId id_;
  QString label_;
  QColor fill_;
  double ghost_dx_px_ = 0.0;
  bool highlighted_ = false;
};

/// Time ruler row across the top of the timeline. Ported from the prototype's
/// TimeRulerItem, rebound to render a core-computed TimelineRuler (tick positions
/// + the chosen interval) rather than recomputing the tick ladder itself. Tick
/// x's are mapped through the same viewport the bars use, so labels line up.
class TimelineRulerItem : public QGraphicsItem {
 public:
  static constexpr double kRulerHeight = 18.0;  // compact header (Blender-style)

  TimelineRulerItem() {
    setZValue(100);
  }

  /// Lay out for the current viewport: `width_px` visible width, `ruler` the
  /// core's tick layout, `epoch_ns` the display-ns subtracted from labels so they
  /// read 0:00 at the scene origin, and [data_min_ns, data_max_ns] the span
  /// covered by data (the union of all datasets) — tinted distinctly from the
  /// empty buffer. Pass data_max <= data_min for "no data" (no tint).
  void setLayout(
      const TimelineViewport& viewport, double width_px, const TimelineRuler& ruler, qint64 epoch_ns,
      qint64 data_min_ns, qint64 data_max_ns) {
    prepareGeometryChange();
    viewport_ = viewport;
    width_px_ = width_px;
    ruler_ = ruler;
    epoch_ns_ = epoch_ns;
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  /// Switch tick labels between elapsed (false) and absolute wall-clock (true).
  /// Pure relabel — tick positions/layout are unchanged, so just repaint.
  void setAbsoluteFormat(bool absolute) {
    if (absolute_ != absolute) {
      absolute_ = absolute;
      update();
    }
  }

  /// Update only the tinted data span (display-ns), keeping the rest of the
  /// layout — used to track the band live during a bar drag without a rebuild.
  void setDataSpan(qint64 data_min_ns, qint64 data_max_ns) {
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    return QRectF(0, 0, width_px_, kRulerHeight);
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const TimelineColors col = timelineColors();
    painter->fillRect(boundingRect(), col.bg);
    // Subtle tint over the data-covered span (earliest start .. latest end — the
    // union/"OR" region) so the ±buffer padding still reads as empty up here too.
    if (data_max_ns_ > data_min_ns_) {
      const double x0 = TimelineScene::nsToPx(data_min_ns_, viewport_);
      const double x1 = TimelineScene::nsToPx(data_max_ns_, viewport_);
      painter->fillRect(QRectF(x0, 0, x1 - x0, kRulerHeight), col.ruler_data);
    }
    // Thin bottom separator; the full-height tick gridlines are drawn by the
    // background item below, so the header itself stays uncluttered.
    painter->setPen(col.ruler_border);
    painter->drawLine(QLineF(0, kRulerHeight - 0.5, width_px_, kRulerHeight - 0.5));

    // Compact frame numbers: small, centered over each tick's gridline, vertically
    // centered in the thin header (Blender-style — no large tick stubs).
    QFont font = painter->font();
    font.setPixelSize(11);
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const double baseline = (kRulerHeight + fm.ascent() - fm.descent()) / 2.0;
    for (const qint64 tick : ruler_.ticks_ns) {
      const double x = TimelineScene::nsToPx(tick, viewport_);
      // The full-height gridline (drawn by the background item, starting flush
      // under this separator) is the tick — the ruler shows only the number,
      // horizontally centered on the gridline (no nub poking up through the
      // separator line). Absolute mode shows the tick's epoch-ns as a unix timestamp.
      const QString label = absolute_ ? formatAbsoluteSeconds(tick) : formatLabel(tick - epoch_ns_);
      const double tw = fm.horizontalAdvance(label);
      painter->setPen(col.text);
      painter->drawText(QPointF(x - (tw / 2.0), baseline), label);
    }
  }

 private:
  static QString formatLabel(qint64 ns_relative) {
    // Pick the format from the value's magnitude (not the tick interval) so labels
    // stay readable whether the visible span is milliseconds or hours.
    const qint64 abs_ns = std::abs(ns_relative);
    if (abs_ns >= kHourNs) {
      const qint64 sec = ns_relative / kSecondNs;
      const qint64 hh = sec / 3600;
      const qint64 mm = (sec / 60) % 60;
      return QString("%1:%2").arg(hh).arg(mm, 2, 10, QChar('0'));
    }
    if (abs_ns >= kMinuteNs) {
      const qint64 sec = ns_relative / kSecondNs;
      const qint64 mm = sec / 60;
      const qint64 ss = sec % 60;
      return QString("%1:%2").arg(mm).arg(ss, 2, 10, QChar('0'));
    }
    if (abs_ns >= kSecondNs) {
      return QString::number(static_cast<double>(ns_relative) / static_cast<double>(kSecondNs), 'f', 1) + "s";
    }
    return QString::number(static_cast<double>(ns_relative) / static_cast<double>(kMillisecondNs), 'f', 1) + "ms";
  }

  TimelineViewport viewport_;
  double width_px_ = 0.0;
  TimelineRuler ruler_;
  qint64 epoch_ns_ = 0;
  qint64 data_min_ns_ = 0;  // data-covered span (display-ns); data_max_<=data_min_ means "no data"
  qint64 data_max_ns_ = 0;
  bool absolute_ = false;  // tick labels: false = elapsed (epoch_ns_-relative), true = wall-clock
};

/// Vertical marker needle, used for both the playhead and the reference line. No
/// top handle/arrow: the line starts at the header bottom (just below the frame
/// numbers — never overlapping the text) and runs down through the rows. Idle it
/// paints `idle_color_`; while grabbed it darkens to `grabbed_color_` and shows a
/// current-time pill in the header, on the SAME baseline/font as the ruler numbers
/// (so the number lines up). A ±6px column stays grabbable for seek-drags.
class TimelineNeedleItem : public QGraphicsItem {
 public:
  static constexpr double kGrabHalfWidth = 6.0;
  static constexpr double kPillHalfWidth = 52.0;  // paint room for the timestamp pill

  TimelineNeedleItem(const QColor& idle_color, const QColor& grabbed_color)
      : idle_color_(idle_color), grabbed_color_(grabbed_color) {
    setFlag(ItemIsSelectable, false);
    setCursor(Qt::SizeHorCursor);
    setAcceptHoverEvents(true);
  }

  void setHeight(double height) {
    if (qFuzzyCompare(height, height_)) {
      return;
    }
    prepareGeometryChange();
    height_ = height;
  }

  /// Scene-y of the (sticky) header's top — the timeline keeps it at the visible
  /// viewport top as the rows scroll, so the time pill and the needle's start
  /// follow the ruler instead of scrolling away. Within [0, height_].
  void setHeaderTop(double scene_y) {
    if (qFuzzyCompare(scene_y, header_top_)) {
      return;
    }
    header_top_ = scene_y;
    update();
  }

  /// Grabbed state: darken to grabbed_color_ + show the timestamp pill.
  void setGrabbed(bool on) {
    if (on == grabbed_) {
      return;
    }
    grabbed_ = on;
    update();
  }

  /// Current-time text for the pill (formatted by the widget).
  void setLabel(const QString& text) {
    if (text == label_) {
      return;
    }
    label_ = text;
    if (grabbed_) {
      update();
    }
  }

  [[nodiscard]] QRectF boundingRect() const override {
    // Wide enough to paint the centered pill; hit area is narrowed in shape().
    return QRectF(-kPillHalfWidth, 0, 2 * kPillHalfWidth, height_);
  }

  // Narrow hit area (just the needle column) so the wide pill region never
  // shadows bars underneath; the pill only appears mid-grab anyway.
  [[nodiscard]] QPainterPath shape() const override {
    QPainterPath path;
    path.addRect(-kGrabHalfWidth, 0, 2 * kGrabHalfWidth, height_);
    return path;
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const QColor color = grabbed_ ? grabbed_color_ : idle_color_;
    const double rh = TimelineRulerItem::kRulerHeight;
    // Start at the (sticky) header bottom so the needle reaches up to — never into —
    // the numbers, even when the rows are scrolled and the header floats down. Use a
    // flat cap so the 2px pen doesn't square-cap a pixel up past the ruler separator.
    QPen needle_pen(color, 2.0);
    needle_pen.setCapStyle(Qt::FlatCap);
    painter->setPen(needle_pen);
    painter->drawLine(QLineF(0, header_top_ + rh, 0, height_));

    if (!grabbed_ || label_.isEmpty()) {
      return;
    }
    // Current-time pill, centered on the needle, sitting in the header on the
    // EXACT same baseline + font as the ruler frame numbers (so it lines up).
    QFont font = painter->font();
    font.setPixelSize(11);
    painter->setFont(font);
    const QFontMetricsF fm(font);
    const double tw = fm.horizontalAdvance(label_);
    const QRectF pill(-tw / 2.0 - 5.0, header_top_ + 0.5, tw + 10.0, rh - 1.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawRoundedRect(pill, 3.0, 3.0);
    const double baseline = header_top_ + ((rh + fm.ascent() - fm.descent()) / 2.0);
    painter->setPen(Qt::white);
    painter->drawText(QPointF(-tw / 2.0, baseline), label_);
  }

 private:
  double height_ = 200.0;
  double header_top_ = 0.0;  // scene-y of the sticky header top (see setHeaderTop)
  bool grabbed_ = false;
  QString label_;
  QColor idle_color_;
  QColor grabbed_color_;
};

/// Full-height background that fills the EMPTY span (outside the data union) with
/// a diagonal stripe hatch, so the ±buffer padding reads as "nothing here" while
/// the data span stays clean. Sits below every other item; never hit-tested.
class TimelineBackgroundItem : public QGraphicsItem {
 public:
  TimelineBackgroundItem() {
    setZValue(-50);
  }

  void setLayout(
      double width, double height, const TimelineViewport& viewport, const TimelineRuler& ruler, qint64 data_min_ns,
      qint64 data_max_ns) {
    prepareGeometryChange();
    width_ = width;
    height_ = height;
    viewport_ = viewport;
    ruler_ = ruler;  // shared with the ruler item so gridlines and numbers can't desync
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  /// Live-update only the data span (during a drag; layout otherwise unchanged).
  void setDataSpan(qint64 data_min_ns, qint64 data_max_ns) {
    data_min_ns_ = data_min_ns;
    data_max_ns_ = data_max_ns;
    update();
  }

  [[nodiscard]] QRectF boundingRect() const override {
    return QRectF(0, 0, width_, height_);
  }

  [[nodiscard]] QPainterPath shape() const override {
    return {};  // decorative; never hit-tested
  }

  void paint(QPainter* painter, const QStyleOptionGraphicsItem* /*option*/, QWidget* /*widget*/) override {
    const TimelineColors col = timelineColors();
    // 1) Solid theme background under everything (white in light theme).
    painter->fillRect(boundingRect(), col.bg);

    // 2) Hatch the empty area (outside the data span) with diagonal lines; the
    // whole timeline when there is no data.
    if (data_max_ns_ <= data_min_ns_) {
      drawHatch(painter, QRectF(0, 0, width_, height_), col.hatch_line);
    } else {
      const double x_lo = TimelineScene::nsToPx(data_min_ns_, viewport_);
      const double x_hi = TimelineScene::nsToPx(data_max_ns_, viewport_);
      if (x_lo > 0.0) {
        drawHatch(painter, QRectF(0, 0, x_lo, height_), col.hatch_line);
      }
      if (x_hi < width_) {
        drawHatch(painter, QRectF(x_hi, 0, width_ - x_hi, height_), col.hatch_line);
      }
    }

    // 3) Faint full-height vertical tick gridlines (the "ticks"), at the SAME
    // tick positions the ruler numbers use — the Timeline computes the ruler once
    // and feeds it to both items via setLayout, so the two can never desync (and
    // we don't re-run the tick ladder on this hot paint path).
    painter->setPen(col.grid_line);
    const double top = TimelineRulerItem::kRulerHeight;
    for (const qint64 tick : ruler_.ticks_ns) {
      const double x = TimelineScene::nsToPx(tick, viewport_);
      painter->drawLine(QLineF(x, top, x, height_));
    }
  }

 private:
  // Evenly-spaced 1px diagonal "/" lines clipped to `rect` — the empty-area
  // texture. Drawn directly (not a tiled brush) so the colour follows the theme.
  static void drawHatch(QPainter* painter, const QRectF& rect, const QColor& color) {
    if (rect.width() <= 0.0 || rect.height() <= 0.0) {
      return;
    }
    painter->save();
    painter->setClipRect(rect);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(color, 1.0));
    constexpr double kSpacing = 14.0;
    const double h = rect.height();
    const double start_x = rect.left() - h;
    const int count = static_cast<int>((rect.width() + h) / kSpacing) + 1;
    for (int i = 0; i < count; ++i) {
      const double xs = start_x + (i * kSpacing);
      painter->drawLine(QLineF(xs, rect.bottom(), xs + h, rect.top()));
    }
    painter->restore();
  }

  double width_ = 0.0;
  double height_ = 0.0;
  TimelineViewport viewport_;
  TimelineRuler ruler_;  // tick list shared with the ruler item (set via setLayout)
  qint64 data_min_ns_ = 0;
  qint64 data_max_ns_ = 0;
};

/// Rubber-band selection rectangle painted in PlotJuggler's light_blue instead
/// of the platform style's default. clearMask() defeats the style's border-only
/// mask so the translucent fill is visible, not just the outline.
class TimelineRubberBand : public QRubberBand {
 public:
  explicit TimelineRubberBand(QWidget* parent) : QRubberBand(QRubberBand::Rectangle, parent) {}

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QRubberBand::resizeEvent(event);
    clearMask();
  }

  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    QColor fill = theme::kLightBlue;
    fill.setAlpha(64);  // translucent so the bars underneath stay visible
    painter.fillRect(rect(), fill);
    painter.setPen(QPen(theme::kLightBlue, 1));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
  }
};

/// Custom horizontal scroll indicator: a small blue "pill" overlaid on the
/// bottom strip of the timeline view, replacing the native scrollbar. Purely
/// visual — transparent for mouse events, so the Timeline keeps a single source
/// of truth for hover/drag (it owns the interaction and feeds the handle rect via
/// setHandle()). Fades in/out via a QGraphicsOpacityEffect the Timeline animates.
class TimelineScrollPill : public QWidget {
 public:
  static constexpr int kAreaThickness = 14;  // strip the pill hovers in (px)
  static constexpr double kThickness = 6.0;  // visual pill thickness (px), centered across the strip

  explicit TimelineScrollPill(QWidget* parent, Qt::Orientation orientation = Qt::Horizontal)
      : QWidget(parent), orientation_(orientation) {
    // The Timeline drives all interaction through the view's event filter, so the
    // overlay must never intercept events itself.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
  }

  /// Handle geometry along the scroll axis, in overlay-local pixels: pos is x (top
  /// for vertical) and length is the handle's extent. length <= 0 paints nothing.
  void setHandle(double pos, double length) {
    handle_pos_ = pos;
    handle_len_ = length;
    update();
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    if (handle_len_ <= 0.0) {
      return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Center the pill across the strip's cross-axis; lay it along the scroll axis.
    QRectF r;
    if (orientation_ == Qt::Horizontal) {
      const double y = (static_cast<double>(height()) - kThickness) / 2.0;
      r = QRectF(handle_pos_, y, handle_len_, kThickness);
    } else {
      const double x = (static_cast<double>(width()) - kThickness) / 2.0;
      r = QRectF(x, handle_pos_, kThickness, handle_len_);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::kBlue);
    painter.drawRoundedRect(r, kThickness / 2.0, kThickness / 2.0);
  }

 private:
  Qt::Orientation orientation_;
  double handle_pos_ = 0.0;
  double handle_len_ = 0.0;
};

/// One name-column row: where to paint it (viewport-local y/height, supplied by
/// the Timeline so it matches the bar row exactly) plus the label and bar color.
struct TimelineNameRow {
  double y_top = 0.0;
  double height = 0.0;
  QString name;
  QColor color;
};

/// Left track-name column. One row per timeline track, pinned on the left so a
/// dataset's name stays visible even when its bar is scrolled out horizontally or
/// squashed thin. The Timeline owns the geometry: it feeds rows in viewport-local
/// y (synced to vertical scroll) so each name aligns exactly with its bar's row.
class TimelineNamePanel : public QWidget {
 public:
  static constexpr int kDefaultWidth = 140;  // initial column width (px); user-resizable
  static constexpr int kMinWidth = 70;       // floor so names never collapse to nothing
  static constexpr int kHeaderHeight = 28;   // top band for the "Datasets" filter; == kRowsTopOffset

  explicit TimelineNamePanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(kMinWidth);
    setMouseTracking(true);  // hover (no button) drives the grab cursor

    // Header band aligned with the ruler on the right — a faithful copy of the
    // curve tree's "Datasets" band: [title][🔍][filter line edit][⋮ kebab]. The
    // band tone, the 2-px title indent, and the transparent inline line edit all
    // come from the shared QSS rules keyed on these object names (see the dark/
    // light stylesheets). WA_StyledBackground so the band's QSS fill paints; it is
    // opaque so rows scrolled up never bleed into the header.
    header_ = new QWidget(this);
    header_->setObjectName(QStringLiteral("timelineDatasetsHeader"));
    header_->setAttribute(Qt::WA_StyledBackground, true);
    auto* row = new QHBoxLayout(header_);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    header_label_ = new QLabel(tr("Datasets"), header_);
    header_label_->setObjectName(QStringLiteral("timelineDatasetsLabel"));

    search_button_ = new QToolButton(header_);
    search_button_->setObjectName(QStringLiteral("timelineDatasetsSearch"));
    search_button_->setAutoRaise(true);
    search_button_->setFocusPolicy(Qt::NoFocus);
    search_button_->setFixedSize(20, 20);
    search_button_->setIconSize(QSize(20, 20));

    filter_edit_ = new QLineEdit(header_);
    filter_edit_->setObjectName(QStringLiteral("timelineDatasetFilter"));
    filter_edit_->setPlaceholderText(tr("Filter..."));
    filter_edit_->setMinimumHeight(24);
    filter_edit_->setMaximumHeight(24);

    row->addWidget(header_label_);
    row->addSpacing(2);
    row->addWidget(search_button_);
    row->addWidget(filter_edit_);
    // Clicking the glass drops focus into the filter, like the curve tree.
    connect(search_button_, &QToolButton::clicked, filter_edit_, [this]() { filter_edit_->setFocus(); });
    // Expand-on-focus, matching the curve tree's filter band: while the filter is
    // focused, hide the "Datasets" title so the inline line edit reclaims its width
    // (restored on blur). Handled in eventFilter().
    filter_edit_->installEventFilter(this);
    applyHeaderTheme();
  }

  /// The filter line edit, so the Timeline can wire its textChanged to the
  /// dataset filter. Never null after construction.
  [[nodiscard]] QLineEdit* filterEdit() const {
    return filter_edit_;
  }

  void setRows(std::vector<TimelineNameRow> rows) {
    rows_ = std::move(rows);
    update();
  }

  /// Drag feedback while a row is being reordered: which row is lifted, the
  /// cursor y the floating copy follows, and the insertion index (0..N).
  void setDrag(int grabbed, double drag_y, int drop_index) {
    drag_grabbed_ = grabbed;
    drag_y_ = drag_y;
    drop_index_ = drop_index;
    update();
  }
  void clearDrag() {
    drag_grabbed_ = -1;
    drop_index_ = -1;
    update();
  }

  [[nodiscard]] int rowCountForTest() const {
    return static_cast<int>(rows_.size());
  }
  [[nodiscard]] double rowTopForTest(int index) const {
    return rows_.at(static_cast<std::size_t>(index)).y_top;
  }

 protected:
  // Expand-on-focus for the filter (mirrors CurveListPanel): hide the "Datasets"
  // title while the line edit is focused so it grows to the full band, restore on blur.
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == filter_edit_ && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
      header_label_->setVisible(event->type() == QEvent::FocusOut);
    }
    return QWidget::eventFilter(watched, event);
  }

  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor window = QGuiApplication::palette().color(QPalette::Window);
    painter.fillRect(rect(), window);
    // The right divider is drawn by the splitter handle (TimelineSplitterHandle).
    const QFontMetricsF fm(font());

    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const TimelineNameRow& row = rows_[static_cast<std::size_t>(i)];
      if (row.y_top + row.height < 0.0 || row.y_top > height()) {
        continue;  // row scrolled out of view
      }
      paintRow(painter, row, row.y_top, fm, i == drag_grabbed_ ? 0.3 : 1.0);
    }

    // Opaque header band so rows scrolled up never bleed into the filter header
    // (the label/search/edit child widgets paint on top of this).
    painter.fillRect(QRectF(0.0, 0.0, width(), kHeaderHeight), window);

    if (drag_grabbed_ < 0 || drag_grabbed_ >= static_cast<int>(rows_.size())) {
      return;
    }
    // Insertion indicator at the drop boundary.
    painter.fillRect(QRectF(0.0, dropLineY() - 1.0, width(), 2.0), theme::kBlue);
    // Floating copy of the grabbed row, opaque, following the cursor.
    const TimelineNameRow& grabbed = rows_[static_cast<std::size_t>(drag_grabbed_)];
    const double fy = drag_y_ - (grabbed.height / 2.0);
    painter.fillRect(QRectF(0.0, fy, width(), grabbed.height), window);
    paintRow(painter, grabbed, fy, fm, 1.0);
    painter.setPen(QPen(theme::kBlue, 1));
    painter.drawRect(QRectF(0.5, fy + 0.5, width() - 1.0, grabbed.height - 1.0));
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    header_->setGeometry(0, 0, width(), kHeaderHeight);  // pin the filter band to the top
  }

  // Re-tint the search glyph when the application theme/palette rolls.
  void changeEvent(QEvent* event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
      applyHeaderTheme();
    }
  }

 private:
  void applyHeaderTheme() {
    const QString theme = currentTheme();
    search_button_->setIcon(loadSvg(QStringLiteral(":/resources/svg/search_light.svg"), theme));
  }

  void paintRow(QPainter& painter, const TimelineNameRow& row, double y_top, const QFontMetricsF& fm, double opacity) {
    constexpr double kAccentWidth = 4.0;  // color strip tying a name to its bar
    constexpr double kTextPad = 8.0;
    painter.setOpacity(opacity);
    painter.fillRect(QRectF(0.0, y_top, kAccentWidth, row.height), row.color);
    const double text_x = kAccentWidth + kTextPad;
    const double text_w = width() - text_x - kTextPad;
    const QString label = fm.elidedText(row.name, Qt::ElideRight, text_w);
    painter.setPen(QGuiApplication::palette().color(QPalette::WindowText));
    painter.drawText(QRectF(text_x, y_top, text_w, row.height), Qt::AlignVCenter | Qt::AlignLeft, label);
    painter.setOpacity(1.0);
  }

  // y of the insertion line for the current drop_index_ (gap midpoint / ends).
  [[nodiscard]] double dropLineY() const {
    if (rows_.empty()) {
      return 0.0;
    }
    if (drop_index_ <= 0) {
      return rows_.front().y_top;
    }
    if (drop_index_ >= static_cast<int>(rows_.size())) {
      return rows_.back().y_top + rows_.back().height;
    }
    const TimelineNameRow& above = rows_[static_cast<std::size_t>(drop_index_) - 1];
    const TimelineNameRow& below = rows_[static_cast<std::size_t>(drop_index_)];
    return ((above.y_top + above.height) + below.y_top) / 2.0;
  }

  std::vector<TimelineNameRow> rows_;
  int drag_grabbed_ = -1;  // lifted row index, -1 = no drag
  double drag_y_ = 0.0;    // cursor y the floating copy tracks
  int drop_index_ = -1;    // insertion index (0..N)
  // Header band widgets (see ctor): the "Datasets" title, search glyph, and the
  // inline filter line edit (which expands over the title while focused).
  QWidget* header_ = nullptr;
  QLabel* header_label_ = nullptr;
  QToolButton* search_button_ = nullptr;
  QLineEdit* filter_edit_ = nullptr;
};

}  // namespace timeline_detail

// =============================================================================
// Timeline — the QWidget (consolidated SourceTimelineWidget).
// =============================================================================

namespace {
constexpr double kRowHeight = 24.0;
constexpr double kRowGap = 2.0;
// Vertical offset of the first dataset row — the blank header band above it (the
// view's ruler is drawn within it on the view side; sized for a standard icon-row
// of breathing room).
constexpr double kRowsTopOffset = 28.0;  // first-dataset vertical offset (px)
constexpr double kMinBarWidthPx = 2.0;
constexpr double kMinScrollPillWidthPx = 28.0;  // floor so a tiny handle stays grabbable
constexpr double kSnapThresholdPx = 8.0;        // catch distance for edge-snap during a drag
constexpr double kSnapReleaseExtraPx = 6.0;     // extra hysteresis band before an active snap releases
const QColor kSnapLineColor(0xFF, 0x6D, 0x00);  // orange alignment guide, distinct from playhead/reference
// Chrono-derived so the seconds<->ns factor can never drift from the unit it
// converts (mirrors PJ::kNanosecondsPerSecond in pj_runtime/Time.h, which
// pj_widgets cannot include per its dependency rule). The widget's public
// playhead/reference/range slots speak display-seconds — the fleet-wide
// IDataWidget/PlaybackEngine double contract — and convert to the ns-native
// interior here.
constexpr double kNanosecondsPerSecond = static_cast<double>(std::chrono::nanoseconds::period::den);
constexpr double kNanosecondsPerMillisecond =
    static_cast<double>(std::chrono::nanoseconds::period::den) / std::chrono::milliseconds::period::den;
constexpr double kZoomInFactor = 1.25;
constexpr double kZoomOutFactor = 1.0 / 1.25;
// Padding added to each side of the content so a source can be dragged a little
// near the edges without the scrollable scene immediately growing. The scene only
// grows past this when a bar is pushed beyond the buffer (see computeBufferedExtent).
constexpr qint64 kBufferNs = 60'000'000'000LL;  // ±1 minute

/// Format a display-relative ns offset as a precise H:MM:SS.mmm-ish timestamp for
/// the marker pills (playhead / reference), more precise than the ruler labels.
QString formatMarkerTime(qint64 rel_ns) {
  const bool neg = rel_ns < 0;
  const qint64 total_ms = std::llround(std::abs(static_cast<double>(rel_ns)) / kNanosecondsPerMillisecond);
  const qint64 ms = total_ms % 1000;
  const qint64 sec = (total_ms / 1000) % 60;
  const qint64 min = (total_ms / 1000) / 60;
  QString body;
  if (min > 0) {
    body = QStringLiteral("%1:%2.%3").arg(min).arg(sec, 2, 10, QChar('0')).arg(ms, 3, 10, QChar('0'));
  } else {
    body = QStringLiteral("%1.%2 s").arg(sec).arg(ms, 3, 10, QChar('0'));
  }
  return (neg ? QStringLiteral("-") : QString()) + body;
}
}  // namespace

using timeline_detail::TimelineBackgroundItem;
using timeline_detail::TimelineBarItem;
using timeline_detail::TimelineNeedleItem;
using timeline_detail::TimelineRulerItem;

Timeline::Timeline(QWidget* parent) : QWidget(parent) {
  gscene_ = new QGraphicsScene(this);
  view_ = new QGraphicsView(gscene_, this);
  view_->setRenderHint(QPainter::Antialiasing, true);
  // The background item paints the themed base over the whole scene; this only
  // shows in any viewport area beyond the scene rect. Kept in step via changeEvent.
  view_->setBackgroundBrush(QGuiApplication::palette().color(QPalette::Window));
  view_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  // Both native scrollbars are hidden in favour of the custom hover pills
  // (scroll_pill_ / vscroll_pill_); AlwaysOff hides the widget but KEEPS each
  // scrollbar's range + value, so panning still flows through scrollBar()->setValue().
  view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setTransformationAnchor(QGraphicsView::NoAnchor);
  // No frame: the viewport's top edge then coincides with the view widget's top,
  // so the left name column (top-aligned beside it) lines up row-for-row.
  view_->setFrameShape(QFrame::NoFrame);
  // Mouse tracking so items get hover events without a button held (drives the
  // per-item cursor: open-hand over bars, resize over the needles).
  view_->viewport()->setMouseTracking(true);
  // Forward the view's mouse/wheel events to this widget's overrides so a single
  // set of handlers drives bar drag, playhead seek, scroll, and zoom.
  view_->viewport()->installEventFilter(this);

  align_button_ = new QPushButton(tr("Align"), this);
  align_button_->setToolTip(tr("Shift every source so their starts line up at the earliest start"));
  connect(align_button_, &QPushButton::clicked, this, &Timeline::alignRequested);

  auto* toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(4, 4, 4, 0);
  toolbar->addWidget(align_button_);
  toolbar->addStretch(1);
  // Host the toolbar in its own widget so it can be hidden as a unit. Hidden for
  // now (the Align action stays wired via alignRequested for when it returns); a
  // hidden widget is skipped by the layout, so no empty row is left above the view.
  auto* toolbar_row = new QWidget(this);
  toolbar_row->setLayout(toolbar);
  toolbar_row->hide();

  // Left name column beside the view, in a splitter so its right edge is a
  // draggable separator the user can pull to widen/narrow the column. The panel
  // paints one row per track aligned to the bars; the view scrolls/zooms while the
  // names stay pinned.
  name_panel_ = new timeline_detail::TimelineNamePanel(this);
  name_panel_->installEventFilter(this);  // central handling of row drag-to-reorder
  // The name column's "Datasets" filter shows only the matching tracks.
  connect(name_panel_->filterEdit(), &QLineEdit::textChanged, this, &Timeline::setDatasetFilter);
  // Plain QSplitter so its handle is the app's standard separator: the global
  // QSS styles QSplitter::handle as a 1-px border line that turns purple on
  // hover/drag, identical to every other splitter divider.
  name_splitter_ = new QSplitter(Qt::Horizontal, this);
  name_splitter_->setObjectName(QStringLiteral("timelineNameSplitter"));
  name_splitter_->setChildrenCollapsible(false);
  name_splitter_->setHandleWidth(1);
  name_splitter_->addWidget(name_panel_);
  name_splitter_->addWidget(view_);
  name_splitter_->setStretchFactor(0, 0);  // the column keeps its width
  name_splitter_->setStretchFactor(1, 1);  // the view absorbs extra space
  name_splitter_->setSizes({timeline_detail::TimelineNamePanel::kDefaultWidth, 1 << 16});
  // Resizing the column shifts the view, so re-place the scroll pill overlay; and
  // tell the host the user's chosen width so it sticks across rebuilds + persists.
  connect(name_splitter_, &QSplitter::splitterMoved, this, [this](int, int) {
    updateScrollPillGeometry();
    emit nameColumnWidthChanged(nameColumnWidth());
  });

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(2);
  layout->addWidget(toolbar_row);
  layout->addWidget(name_splitter_);

  background_item_ = new TimelineBackgroundItem();
  gscene_->addItem(background_item_);

  ruler_item_ = new TimelineRulerItem();
  gscene_->addItem(ruler_item_);

  // Playback needle: magenta idle (theme::kPurple — the SAME magenta as the plot
  // widget's playback tracker line, for cross-widget visual consistency), darker
  // magenta when grabbed. Reference needle: light blue idle, dark blue when
  // grabbed, hidden until a reference line is toggled on by the host.
  playhead_item_ = new TimelineNeedleItem(theme::kPurple, theme::kPurpleDark);
  playhead_item_->setZValue(200);
  gscene_->addItem(playhead_item_);

  reference_item_ = new TimelineNeedleItem(theme::kLightBlue, theme::kBlue);
  reference_item_->setZValue(190);  // just under the playback needle
  reference_item_->setVisible(false);
  gscene_->addItem(reference_item_);

  // Edge-snap alignment guide: a thin vertical line shown while a dragged bar's
  // edge snaps to a neighbour's edge. Above the bars, below the playback needle.
  snap_line_item_ = new QGraphicsLineItem();
  snap_line_item_->setZValue(180);
  snap_line_item_->setPen(QPen(kSnapLineColor, 1.0));
  snap_line_item_->setVisible(false);
  gscene_->addItem(snap_line_item_);

  // Custom scroll pill overlay. Lives on top of the view (a sibling child of this
  // widget, positioned manually over the viewport's bottom strip — never in the
  // layout). Starts fully faded out; the hover logic fades it in.
  scroll_pill_ = new timeline_detail::TimelineScrollPill(this);
  pill_opacity_ = new QGraphicsOpacityEffect(scroll_pill_);
  pill_opacity_->setOpacity(0.0);
  scroll_pill_->setGraphicsEffect(pill_opacity_);
  scroll_pill_->show();
  scroll_pill_->raise();
  pill_fade_ = new QPropertyAnimation(pill_opacity_, "opacity", this);
  pill_fade_->setDuration(150);  // gentle fade in/out

  // Vertical twin of the scroll pill, on the viewport's right strip (mirrors the
  // hidden vertical scrollbar). Same overlay treatment + fade.
  vscroll_pill_ = new timeline_detail::TimelineScrollPill(this, Qt::Vertical);
  vpill_opacity_ = new QGraphicsOpacityEffect(vscroll_pill_);
  vpill_opacity_->setOpacity(0.0);
  vscroll_pill_->setGraphicsEffect(vpill_opacity_);
  vscroll_pill_->show();
  vscroll_pill_->raise();
  vpill_fade_ = new QPropertyAnimation(vpill_opacity_, "opacity", this);
  vpill_fade_->setDuration(150);

  // "Frozen" overlay: a centered pill shown over the scene while the timeline is
  // interaction-locked (live streaming + playing), telling the user how to interact.
  // Purely informational — WA_TransparentForMouseEvents so it never changes event
  // routing (the press/wheel gates already block interaction). Hidden until locked.
  lock_overlay_ = new QLabel(tr("Streaming: pause playback to interact with the timeline"), this);
  lock_overlay_->setObjectName(QStringLiteral("timelineLockOverlay"));
  lock_overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
  lock_overlay_->setAlignment(Qt::AlignCenter);
  lock_overlay_->setWordWrap(true);
  // Rounded grey pill centered over the scene (geometry set in
  // updateLockOverlayGeometry): sized to the text + padding, NOT a full-width band —
  // prominent enough to read at a glance while leaving the streaming bars visible.
  lock_overlay_->setStyleSheet(QStringLiteral(
      "QLabel#timelineLockOverlay { background-color: rgba(120, 120, 120, 225); color: #f7f7f7; "
      "border: 1px solid rgba(255, 255, 255, 55); border-radius: 10px; padding: 12px 28px; "
      "font-size: 18px; font-weight: 600; }"));
  lock_overlay_->hide();

  // Keep both pills glued to their scrollbar handles when panning/zoom/resize change them.
  connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) { updateScrollPillGeometry(); });
  connect(
      view_->horizontalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updateScrollPillGeometry(); });
  connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
    updateNamePanel();
    syncStickyHeader();  // keep the number line pinned to the top as the rows scroll
    updateScrollPillGeometry();
  });
  connect(
      view_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) { updateScrollPillGeometry(); });

  rebuild();
}

Timeline::~Timeline() {
  // Drop the scrollbar connections before our members tear down. ~QWidget (the
  // base, run after our members are gone) deletes the child QGraphicsScene, which
  // resets the view's scrollbars and emits valueChanged — that would otherwise
  // fire the lambdas into updateNamePanel()/syncStickyHeader() after scene_ et al.
  // are already destroyed. (The `this`-context auto-disconnect only kicks in once
  // ~QObject runs, which is too late.)
  if (view_ != nullptr) {
    view_->horizontalScrollBar()->disconnect(this);
    view_->verticalScrollBar()->disconnect(this);
  }
}

int Timeline::barCount() const {
  return static_cast<int>(bar_items_.size());
}

void Timeline::setTracks(const std::vector<TimelineTrack>& tracks) {
  all_tracks_ = tracks;
  // Hash the RAW data (id + raw bounds, NOT offsets) of the FULL list to tell a
  // genuine data change (load/remove/reload) from an offset-only update (a drag).
  // Computing it from the full list — not the filtered subset — means the dataset
  // filter never resets the extent or triggers auto-zoom. The combine is order-
  // INDEPENDENT (XOR of per-track hashes) so a pure reorder leaves it unchanged.
  quint64 sig = 0;
  for (const TimelineTrack& t : all_tracks_) {
    quint64 h = 1469598103934665603ULL;  // FNV-1a offset basis
    const auto mix = [&h](quint64 v) { h = (h ^ v) * 1099511628211ULL; };
    mix(t.id);
    mix(static_cast<quint64>(t.t_min_ns));
    mix(static_cast<quint64>(t.t_max_ns));
    sig ^= h;
  }
  if (sig != raw_data_sig_) {
    raw_data_sig_ = sig;
    extent_needs_reset_ = true;  // re-pad the scene from the new data bounds
    selected_ids_.clear();       // a real data change invalidates the old selection
  }
  applyDatasetFilter();
}

void Timeline::setDatasetFilter(const QString& text) {
  if (text == dataset_filter_) {
    return;
  }
  dataset_filter_ = text;
  // A filter change re-selects which tracks are shown but is NOT a data change, so
  // it doesn't touch raw_data_sig_/extent_needs_reset_ — the view stays put.
  applyDatasetFilter();
}

void Timeline::applyDatasetFilter() {
  // Feed only the filter-matching tracks into the scene; the bars, name rows,
  // extent, and reorder all operate on this filtered subset, so the rest of the
  // widget needs no filter awareness. Case-insensitive substring match on the
  // dataset name; an empty filter shows everything.
  std::vector<TimelineSpanInput> spans;
  bar_colors_.clear();
  bar_labels_.clear();
  for (const TimelineTrack& t : all_tracks_) {
    if (!dataset_filter_.isEmpty() && !t.name.contains(dataset_filter_, Qt::CaseInsensitive)) {
      continue;
    }
    spans.push_back(
        TimelineSpanInput{.id = t.id, .t_min_ns = t.t_min_ns, .t_max_ns = t.t_max_ns, .offset_ns = t.offset_ns});
    bar_colors_.push_back(t.color);
    bar_labels_.push_back(t.name);
  }
  scene_.setTracks(std::move(spans));
  rebuild();
}

void Timeline::setPlayhead(double display_seconds) {
  // `display_seconds` arrives in the host's external (playback) frame; shift into
  // the bars' internal frame by adding the offset in INTEGER ns (never via an
  // epoch-scale double add, which would lose ~one ulp ≈ 240 ns of precision).
  playhead_ns_ = static_cast<qint64>(std::llround(display_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_;
  repositionPlayhead();
  // The needle is a pure slave (seekToSceneX no longer sets the pill), so update the
  // grabbed time pill here from the echoed engine value while a drag is in progress.
  if (dragging_playhead_) {
    playhead_item_->setLabel(markerLabel(playhead_ns_));
  }
}

QString Timeline::markerLabel(qint64 display_ns) const {
  // fixed_ms=true so the pill matches the playback readout (always 3 decimals).
  return absolute_time_labels_ ? timeline_detail::formatAbsoluteSeconds(display_ns, /*fixed_ms=*/true)
                               : formatMarkerTime(display_ns - ruler_epoch_ns_);
}

void Timeline::setAbsoluteTimeLabels(bool absolute) {
  if (absolute_time_labels_ == absolute) {
    return;
  }
  absolute_time_labels_ = absolute;
  ruler_item_->setAbsoluteFormat(absolute);
  // Refresh the (lazily-shown) pill labels so the next grab reads the new format.
  playhead_item_->setLabel(markerLabel(playhead_ns_));
  reference_item_->setLabel(markerLabel(reference_ns_));
}

void Timeline::setTimeFrameOffsetNs(qint64 offset_ns) {
  // The ns gap between the host's external (playback) frame — in which the playhead,
  // range, and reference are supplied/emitted — and the bars' internal frame. Adding
  // it in integer ns (here and in the setters) keeps the needle bit-exact with the
  // playback instant; doing the same shift in epoch-scale double seconds would round
  // to ~240 ns and make the timeline and playback readouts disagree on the last digit.
  // Bars are fed in the internal frame directly (setTracks), so they're unaffected.
  const qint64 delta = offset_ns - time_frame_offset_ns_;
  if (delta == 0) {
    return;
  }
  time_frame_offset_ns_ = offset_ns;
  // The playhead/reference hold a fixed playback INSTANT; re-express it in the new bar
  // frame so the needle doesn't strand at the old position when the offset moves (a
  // load/toggle changing the global reference). The host often does NOT re-push
  // currentTime on a load (it may be unchanged), so this self-correction is required.
  // setPlayhead/setReferenceLine still overwrite absolutely, so this never double-applies.
  playhead_ns_ += delta;
  reference_ns_ += delta;
  repositionPlayhead();
  repositionReference();
}

void Timeline::setDisplayRange(double lo_seconds, double hi_seconds) {
  // When tracks are present the displayed extent comes from their data; this
  // range only governs the EMPTY timeline, which we make exactly as long as the
  // playback by adopting [lo, hi] as the empty-state span (rebuild() then fits it
  // to the view width — see the empty-fit block there). Stored on the scene so
  // every sceneExtent() caller (extent, ruler epoch) sees the same empty span.
  scene_.setDefaultExtent(
      static_cast<qint64>(std::llround(lo_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_,
      static_cast<qint64>(std::llround(hi_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_);
  rebuild();
}

bool Timeline::isAutoZoomEnabled() const noexcept {
  return auto_zoom_;
}

void Timeline::setAutoZoomEnabled(bool enabled) {
  if (auto_zoom_ == enabled) {
    return;
  }
  auto_zoom_ = enabled;
  if (enabled) {
    fitToContents();  // re-frame the current extent immediately on enable
  }
}

void Timeline::fitToContents() {
  // Request a fit on the next rebuild; rebuild() honors it only when auto-zoom is
  // on and there is data (the empty timeline always fits the playback range).
  fit_pending_ = true;
  rebuild();
}

void Timeline::zoomToFit() {
  // Explicit, one-shot "zoom out horizontally" — fits the largest extent into the
  // view regardless of the auto-zoom preference.
  force_fit_pending_ = true;
  rebuild();
}

double Timeline::zoom() const noexcept {
  return viewport_.px_per_ns;
}

void Timeline::setZoom(double px_per_ns) {
  if (!(px_per_ns > 0.0)) {
    return;  // ignore non-positive / NaN
  }
  viewport_.px_per_ns = std::clamp(px_per_ns, kMinPxPerNs, kMaxPxPerNs);
  rebuild();  // scene width + scrollbar range track the zoom
}

qint64 Timeline::viewportLeftDisplayNs() const {
  // The horizontal scrollbar value is the scene-x of the visible left edge; map
  // it back to display-ns under the current viewport (origin pinned at scene x=0).
  if (view_ == nullptr || view_->horizontalScrollBar() == nullptr) {
    return 0;
  }
  return TimelineScene::pxToNs(static_cast<double>(view_->horizontalScrollBar()->value()), viewport_);
}

void Timeline::setViewportLeftDisplayNs(qint64 display_ns) {
  if (view_ == nullptr || view_->horizontalScrollBar() == nullptr) {
    return;
  }
  // QScrollBar::setValue clamps to [min,max], so an out-of-range position (e.g.
  // a wider window than at save time) lands at the nearest reachable edge.
  const double scene_x = TimelineScene::nsToPx(display_ns, viewport_);
  view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(scene_x)));
}

int Timeline::nameColumnWidth() const {
  if (name_splitter_ == nullptr) {
    return 0;
  }
  const QList<int> sizes = name_splitter_->sizes();
  return sizes.isEmpty() ? 0 : sizes.front();
}

void Timeline::setNameColumnWidth(int width_px) {
  if (name_panel_ == nullptr || width_px <= 0) {
    return;
  }
  // Floor the column at this width (it can be dragged wider, never narrower) and
  // pin the splitter so the separator lands exactly at width_px by default.
  name_panel_->setMinimumWidth(width_px);
  resizeNameColumn(width_px);
}

void Timeline::resizeNameColumn(int width_px) {
  if (name_splitter_ == nullptr || width_px <= 0) {
    return;
  }
  const QList<int> sizes = name_splitter_->sizes();
  if (sizes.size() == 2) {
    const int total = sizes[0] + sizes[1];
    name_splitter_->setSizes({width_px, std::max(0, total - width_px)});
  }
}

double Timeline::sceneHeight() const {
  const double rows_h = static_cast<double>(scene_.tracks().size()) * (kRowHeight + kRowGap);
  const double vp_h =
      (view_->viewport() && view_->viewport()->height() > 0) ? static_cast<double>(view_->viewport()->height()) : 100.0;
  return std::max(vp_h, kRowsTopOffset + rows_h);
}

void Timeline::rebuild() {
  // Tearing down items mid-drag would dangle the drag-group items and crash the
  // next move. The host's setTracks() after release triggers a normal rebuild.
  if (!drag_group_.empty()) {
    return;
  }

  for (TimelineBarItem* item : bar_items_) {
    gscene_->removeItem(item);
    delete item;
  }
  bar_items_.clear();

  // Stable across this rebuild; compute the union extent once and reuse it below.
  const TimeSpan scene_extent = scene_.sceneExtent();

  const bool did_reset = extent_needs_reset_;
  buffered_extent_ = computeBufferedExtent();
  extent_needs_reset_ = false;
  // The horizontal scrollbar owns panning, so the origin (display-ns at scene
  // x=0) is pinned to the scene's left edge here — never moved by zoom/scroll.
  viewport_.origin_ns = buffered_extent_.min;
  // Anchor the ruler's 0:00 reference ONCE, when the data changes — not on a
  // drag. This keeps the timing headers stationary while a dataset is moved (the
  // axis is an absolute frame; only the dragged bar moves).
  if (did_reset) {
    ruler_epoch_ns_ = scene_extent.min;
  }

  const double vp_w =
      (view_->viewport() && view_->viewport()->width() > 0) ? static_cast<double>(view_->viewport()->width()) : 100.0;
  // Auto-fit the zoom to the view width. Both cases are gated on isVisible() so
  // they never run during the constructor's pre-show rebuild (whose fallback
  // width would bake a bogus zoom into viewport_ that the first dataset inherits):
  //   * Empty timeline: ALWAYS fit the playback range (buffered_extent_), so the
  //     data-less timeline is exactly as long as the playback.
  //   * Data timeline: fit the largest extent (the union of all bars) when
  //     auto-zoom is on AND new data just arrived (did_reset) or a fit was asked
  //     for (fit_pending_, e.g. after an alignment). Otherwise it keeps the user's
  //     zoom.
  // Re-runs on resize (resizeEvent -> rebuild) so the empty fit stays snug.
  const bool empty = scene_.tracks().empty();
  // force_fit_pending_ is an explicit user "zoom to fit" (the align rail's
  // zoom-out-horizontally button); it fits regardless of the auto-zoom preference.
  const bool do_fit =
      view_->isVisible() && (empty || force_fit_pending_ || (auto_zoom_ && (did_reset || fit_pending_)));
  if (do_fit && vp_w > 0.0) {
    const TimeSpan fit_span = empty ? buffered_extent_ : scene_extent;
    const qint64 span_ns = fit_span.max - fit_span.min;
    if (span_ns > 0) {
      // Clamp to the same floor/ceiling every other zoom path enforces (zoom(),
      // setZoom()); a degenerate (huge) span would otherwise store a sub-floor zoom.
      viewport_.px_per_ns = std::clamp(vp_w / static_cast<double>(span_ns), kMinPxPerNs, kMaxPxPerNs);
    }
  }
  fit_pending_ = false;
  force_fit_pending_ = false;
  const double extent_px = static_cast<double>(buffered_extent_.max - buffered_extent_.min) * viewport_.px_per_ns;
  const double scene_w = std::max(vp_w, extent_px);
  const double scene_h = sceneHeight();
  gscene_->setSceneRect(0, 0, scene_w, scene_h);

  // Compute the ruler tick layout ONCE per rebuild and feed it to both the
  // background gridlines and the ruler numbers, so the two share a single tick
  // list (no per-paint recompute, no comment-only "same positions" contract).
  const TimelineRuler ruler = TimelineScene::ruler(viewport_, scene_w);
  // The data union span both tints the ruler and bounds the empty-area hatch.
  const TimeSpan data_span = scene_.tracks().empty() ? TimeSpan{0, 0} : scene_extent;
  background_item_->setLayout(scene_w, scene_h, viewport_, ruler, data_span.min, data_span.max);

  double y = kRowsTopOffset;
  const std::vector<TimelineSpanInput>& tracks = scene_.tracks();
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const TimelineSpanInput& track = tracks[i];
    const PxSpan span = TimelineScene::barSpan(track, viewport_);
    const double width = std::max(span.width, kMinBarWidthPx);
    const QColor color = (i < bar_colors_.size()) ? bar_colors_[i] : QColor(Qt::white);
    const QString label = (i < bar_labels_.size()) ? bar_labels_[i] : QString();
    auto* item = new TimelineBarItem(track.id, label, color);
    item->setRect(0, 0, width, kRowHeight);
    item->setPos(span.x, y);
    item->setHighlighted(selected_ids_.count(track.id) != 0);
    gscene_->addItem(item);
    bar_items_.push_back(item);
    y += kRowHeight + kRowGap;
  }

  rebuildRuler(ruler, data_span);
  playhead_item_->setHeight(scene_h);
  reference_item_->setHeight(scene_h);
  repositionPlayhead();
  repositionReference();

  // Whenever we (re-)fit — fresh data, an empty timeline, or an explicit
  // fitToContents (alignment) — scroll so the earliest content sits at the left
  // edge: data fills the view from its start (±1 min buffer reachable by scrolling
  // left), and the empty timeline opens with 0:00 flush at the left edge (under
  // the playback track start). Only on a fit, so it never fights user scrolling.
  if (did_reset || do_fit) {
    const qint64 content_min = scene_extent.min;
    view_->horizontalScrollBar()->setValue(
        static_cast<int>(std::llround(TimelineScene::nsToPx(content_min, viewport_))));
  }

  updateNamePanel();   // names follow the same row layout the bars just got
  syncStickyHeader();  // pin the ruler + needle pills to the current viewport top
}

void Timeline::syncStickyHeader() {
  if (ruler_item_ == nullptr) {
    return;
  }
  // The viewport top maps to scene-y == verticalScrollBar value (no vertical
  // scaling). Keep the ruler (and the needles' header anchor) there so the rows
  // scroll underneath a stationary number line.
  const auto vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  ruler_item_->setPos(0, vscroll);
  if (playhead_item_ != nullptr) {
    playhead_item_->setHeaderTop(vscroll);
  }
  if (reference_item_ != nullptr) {
    reference_item_->setHeaderTop(vscroll);
  }
}

TimeSpan Timeline::computeBufferedExtent() const {
  const TimeSpan content = scene_.sceneExtent();
  // Empty timeline: span EXACTLY the default extent (the playback range), with no
  // buffer and no sticky growth — so it is always exactly as long as the playback
  // (tracking setDisplayRange even when extent_needs_reset_ is false) and the
  // empty-fit in rebuild() makes that span fill the view, 0ms flush at the left
  // edge. The sticky-buffer logic below is only meaningful while bars exist.
  if (scene_.tracks().empty()) {
    return content;
  }
  if (extent_needs_reset_) {
    // Fresh data: pad ±1 min around the content.
    return {content.min - kBufferNs, content.max + kBufferNs};
  }
  // Offset-only update (a drag): keep the current padded extent so small drags
  // inside the buffer don't resize the scene. Re-pad only the side a bar was
  // pushed past, so the extent grows (never shrinks) when dragged beyond.
  TimeSpan out = buffered_extent_;
  if (content.min < out.min) {
    out.min = content.min - kBufferNs;
  }
  if (content.max > out.max) {
    out.max = content.max + kBufferNs;
  }
  return out;
}

void Timeline::updateScrollPillGeometry() {
  const QWidget* vp = view_->viewport();
  const int strip = timeline_detail::TimelineScrollPill::kAreaThickness;

  // Maps a scrollbar onto a pill handle: the total slider span is range + page ==
  // the content extent in px; the handle covers [value, value+page]. Returns the
  // handle's start + length in track pixels, or {0,0} when there's nothing to scroll.
  const auto handle_for = [](const QScrollBar* bar, double track_len) -> std::pair<double, double> {
    const double range = static_cast<double>(bar->maximum() - bar->minimum());
    const double page = static_cast<double>(bar->pageStep());
    if (range <= 0.0 || page <= 0.0) {
      return {0.0, 0.0};
    }
    const double total = range + page;
    const double len = std::max(kMinScrollPillWidthPx, (page / total) * track_len);
    const double frac = static_cast<double>(bar->value() - bar->minimum()) / total;
    const double pos = std::clamp(frac * track_len, 0.0, std::max(0.0, track_len - len));
    return {pos, len};
  };

  // Horizontal pill: bottom strip across the full viewport width.
  if (scroll_pill_ != nullptr) {
    const QPoint tl = vp->mapTo(this, QPoint(0, vp->height() - strip));
    scroll_pill_->setGeometry(tl.x(), tl.y(), vp->width(), strip);
    const auto handle = handle_for(view_->horizontalScrollBar(), static_cast<double>(vp->width()));
    pill_x_ = handle.first;
    pill_w_ = handle.second;
    scroll_pill_->setHandle(pill_x_, pill_w_);
  }
  // Vertical pill: right strip down the full viewport height.
  if (vscroll_pill_ != nullptr) {
    const QPoint tl = vp->mapTo(this, QPoint(vp->width() - strip, 0));
    vscroll_pill_->setGeometry(tl.x(), tl.y(), strip, vp->height());
    const auto handle = handle_for(view_->verticalScrollBar(), static_cast<double>(vp->height()));
    vpill_y_ = handle.first;
    vpill_h_ = handle.second;
    vscroll_pill_->setHandle(vpill_y_, vpill_h_);
  }
  // The lock overlay rides over the same viewport, so recenter it on the same triggers
  // (resize, zoom/pan range changes, splitter drag) that move the pills.
  updateLockOverlayGeometry();
}

void Timeline::updateLockOverlayGeometry() {
  if (lock_overlay_ == nullptr) {
    return;
  }
  const QWidget* vp = view_->viewport();
  // Centered rounded pill sized to its text. adjustSize() alone mis-sizes a
  // word-wrapped QLabel into a tall narrow box, so the width is set explicitly (the
  // single-line natural width, capped to the viewport so a narrow timeline wraps
  // instead of overflowing) and adjustSize only derives the matching height.
  constexpr int kMargin = 24;
  const int max_w = std::max(120, vp->width() - 2 * kMargin);
  lock_overlay_->setWordWrap(false);
  const int natural_w = lock_overlay_->sizeHint().width();
  lock_overlay_->setWordWrap(natural_w > max_w);
  lock_overlay_->setFixedWidth(std::min(natural_w, max_w));
  lock_overlay_->adjustSize();
  const int x = (vp->width() - lock_overlay_->width()) / 2;
  const int y = (vp->height() - lock_overlay_->height()) / 2;
  lock_overlay_->move(vp->mapTo(this, QPoint(std::max(0, x), std::max(0, y))));
}

void Timeline::setScrollPillShown(bool shown) {
  const QScrollBar* hbar = view_->horizontalScrollBar();
  // Ignore a show request when there's nothing to scroll (matches ScrollBarAsNeeded).
  const bool want = shown && (hbar->maximum() > hbar->minimum());
  if (want == pill_shown_) {
    return;
  }
  pill_shown_ = want;
  if (want) {
    updateScrollPillGeometry();
    scroll_pill_->raise();
  }
  pill_fade_->stop();
  pill_fade_->setStartValue(pill_opacity_->opacity());
  pill_fade_->setEndValue(want ? 1.0 : 0.0);
  pill_fade_->start();
}

void Timeline::setVScrollPillShown(bool shown) {
  const QScrollBar* vbar = view_->verticalScrollBar();
  const bool want = shown && (vbar->maximum() > vbar->minimum());
  if (want == vpill_shown_) {
    return;
  }
  vpill_shown_ = want;
  if (want) {
    updateScrollPillGeometry();
    vscroll_pill_->raise();
  }
  vpill_fade_->stop();
  vpill_fade_->setStartValue(vpill_opacity_->opacity());
  vpill_fade_->setEndValue(want ? 1.0 : 0.0);
  vpill_fade_->start();
}

bool Timeline::pointInPillArea(const QPoint& viewport_pos) const {
  const QWidget* vp = view_->viewport();
  const int strip_top = vp->height() - timeline_detail::TimelineScrollPill::kAreaThickness;
  return viewport_pos.y() >= strip_top && viewport_pos.x() >= 0 && viewport_pos.x() <= vp->width();
}

bool Timeline::pointInVPillArea(const QPoint& viewport_pos) const {
  const QWidget* vp = view_->viewport();
  const int strip_left = vp->width() - timeline_detail::TimelineScrollPill::kAreaThickness;
  return viewport_pos.x() >= strip_left && viewport_pos.y() >= 0 && viewport_pos.y() <= vp->height();
}

void Timeline::updateNamePanel() {
  if (name_panel_ == nullptr) {
    return;
  }
  // Each row's scene y is the bar row's y; subtract the vertical scroll so the
  // panel (top-aligned with the frameless viewport) lines up row-for-row.
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const std::vector<TimelineSpanInput>& tracks = scene_.tracks();
  std::vector<timeline_detail::TimelineNameRow> rows;
  rows.reserve(tracks.size());
  double y = kRowsTopOffset;
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    timeline_detail::TimelineNameRow row;
    row.y_top = y - vscroll;
    row.height = kRowHeight;
    row.name = (i < bar_labels_.size()) ? bar_labels_[i] : QString();
    row.color = (i < bar_colors_.size()) ? bar_colors_[i] : QColor(Qt::white);
    rows.push_back(std::move(row));
    y += kRowHeight + kRowGap;
  }
  name_panel_->setRows(std::move(rows));
}

int Timeline::nameRowAt(double panel_y) const {
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const int n = static_cast<int>(scene_.tracks().size());
  for (int i = 0; i < n; ++i) {
    const double top = (kRowsTopOffset + (i * (kRowHeight + kRowGap))) - vscroll;
    if (panel_y >= top && panel_y < top + kRowHeight) {
      return i;
    }
  }
  return -1;
}

int Timeline::nameDropIndex(double panel_y) const {
  const double vscroll = static_cast<double>(view_->verticalScrollBar()->value());
  const int n = static_cast<int>(scene_.tracks().size());
  for (int i = 0; i < n; ++i) {
    const double center = (kRowsTopOffset + (i * (kRowHeight + kRowGap)) + (kRowHeight / 2.0)) - vscroll;
    if (panel_y < center) {
      return i;  // insert before row i
    }
  }
  return n;  // after the last row
}

void Timeline::namePanelPress(const QPoint& panel_pos) {
  if (interaction_locked_) {
    name_drag_index_ = -1;  // Locked: no row reorder.
    return;
  }
  name_drag_index_ = (scene_.tracks().size() >= 2) ? nameRowAt(panel_pos.y()) : -1;
  name_dragging_ = false;
  name_drag_press_y_ = panel_pos.y();
}

void Timeline::namePanelMove(const QPoint& panel_pos, bool button_down) {
  if (!button_down || name_drag_index_ < 0) {
    // Hover: show the grab cursor over a row when reordering is possible.
    const bool grabbable = scene_.tracks().size() >= 2 && nameRowAt(panel_pos.y()) >= 0;
    name_panel_->setCursor(grabbable ? Qt::OpenHandCursor : Qt::ArrowCursor);
    return;
  }
  constexpr double kDragThresholdPx = 4.0;
  if (!name_dragging_ && std::abs(panel_pos.y() - name_drag_press_y_) < kDragThresholdPx) {
    return;  // not enough movement to start a drag yet
  }
  name_dragging_ = true;
  name_panel_->setCursor(Qt::ClosedHandCursor);
  name_panel_->setDrag(name_drag_index_, panel_pos.y(), nameDropIndex(panel_pos.y()));
}

void Timeline::namePanelRelease(const QPoint& panel_pos) {
  const int from = name_drag_index_;
  const bool was_dragging = name_dragging_;
  name_drag_index_ = -1;
  name_dragging_ = false;
  name_panel_->clearDrag();
  name_panel_->setCursor(nameRowAt(panel_pos.y()) >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
  if (was_dragging && from >= 0) {
    applyTrackReorder(from, nameDropIndex(panel_pos.y()));
  }
}

void Timeline::applyTrackReorder(int from, int drop_index) {
  if (interaction_locked_) {
    return;  // Locked: track order is frozen.
  }
  const std::vector<TimelineSpanInput>& spans = scene_.tracks();
  const int n = static_cast<int>(spans.size());
  if (from < 0 || from >= n || n < 2) {
    return;
  }
  int ins = std::clamp(drop_index, 0, n);
  if (ins > from) {
    --ins;  // the removed row shifts later indices down by one
  }
  if (ins == from) {
    return;  // dropped back where it started
  }
  // New index order: all rows except `from`, with `from` re-inserted at `ins`.
  std::vector<int> order;
  order.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    if (i != from) {
      order.push_back(i);
    }
  }
  order.insert(order.begin() + ins, from);
  // Reconstruct the track list in the new order and re-feed via setTracks. The
  // data signature is order-independent, so this re-lays-out the rows without
  // resetting the extent/scroll. Then announce the order for the host to adopt.
  std::vector<TimelineTrack> reordered;
  reordered.reserve(static_cast<std::size_t>(n));
  QList<quint64> ids;
  for (int idx : order) {
    const TimelineSpanInput& s = spans[static_cast<std::size_t>(idx)];
    const auto u_idx = static_cast<std::size_t>(idx);
    TimelineTrack track;
    track.id = s.id;
    track.t_min_ns = s.t_min_ns;
    track.t_max_ns = s.t_max_ns;
    track.offset_ns = s.offset_ns;
    track.name = (u_idx < bar_labels_.size()) ? bar_labels_[u_idx] : QString();
    track.color = (u_idx < bar_colors_.size()) ? bar_colors_[u_idx] : QColor(Qt::white);
    reordered.push_back(std::move(track));
    ids.push_back(s.id);
  }
  setTracks(reordered);
  emit tracksReordered(ids);
}

void Timeline::rebuildRuler(const TimelineRuler& ruler, const TimeSpan& data) {
  const double width_px = gscene_->sceneRect().width();
  // `data` is the data-covered span (union of all datasets, or an empty span when
  // there are no tracks) rebuild() already computed — it tints the ruler distinctly
  // from the empty ±buffer.
  // Labels are relative to the FIXED epoch (set on data load), so the timing
  // headers stay put while a dataset is dragged.
  ruler_item_->setLayout(viewport_, width_px, ruler, ruler_epoch_ns_, data.min, data.max);
  // Keep the ruler at the current viewport top (sticky); rebuild() also calls
  // syncStickyHeader() after this, but positioning here keeps rebuildRuler correct
  // on its own.
  ruler_item_->setPos(0, static_cast<double>(view_->verticalScrollBar()->value()));
}

void Timeline::updateDataSpanFromItems() {
  if (bar_items_.empty()) {
    ruler_item_->setDataSpan(0, 0);
    background_item_->setDataSpan(0, 0);
    return;
  }
  // Union the bars' current VISUAL extents (item position + the in-flight drag's
  // ghost dx), so the ruler tint AND the empty-area hatch track the dragged
  // bar(s) live. Reading the items (not scene_) keeps this correct whether or
  // not a host has fed the offset back.
  double min_x = std::numeric_limits<double>::max();
  double max_x = std::numeric_limits<double>::lowest();
  for (const TimelineBarItem* bar : bar_items_) {
    const double x0 = bar->pos().x() + bar->ghostDx();
    const double x1 = x0 + bar->rect().width();
    min_x = std::min(min_x, x0);
    max_x = std::max(max_x, x1);
  }
  const qint64 lo = TimelineScene::pxToNs(min_x, viewport_);
  const qint64 hi = TimelineScene::pxToNs(max_x, viewport_);
  ruler_item_->setDataSpan(lo, hi);
  background_item_->setDataSpan(lo, hi);
}

void Timeline::repositionPlayhead() {
  const double x = TimelineScene::nsToPx(playhead_ns_, viewport_);
  playhead_item_->setPos(x, 0);
}

double Timeline::seekToSceneX(double scene_x) {
  if (interaction_locked_) {
    // Locked: the needle is view-only — report the current playhead (external
    // frame) without emitting a seek, so callers/test seams see no movement.
    return static_cast<double>(playhead_ns_ - time_frame_offset_ns_) / kNanosecondsPerSecond;
  }
  // Clamp to the data extent (union of the bars, or the playback range when empty)
  // so an out-of-range drag emits a sane value. The needle is a PURE SLAVE: we do
  // NOT move playhead_ns_ here. The host writes this time to the PlaybackEngine, and
  // its currentTimeChanged echo (setPlayhead, synchronous) is the SINGLE source that
  // repositions the needle — so it stays exactly in lockstep with the playback handle
  // (same source, same value) instead of holding a locally-set value the engine never
  // adopted.
  const TimeSpan extent = scene_.sceneExtent();
  const qint64 ns = std::clamp(TimelineScene::pxToNs(scene_x, viewport_), extent.min, extent.max);
  // Emit in the host's external (playback) frame: undo the frame offset in integer
  // ns first, then divide — the difference is small, so the double stays precise.
  const double seconds = static_cast<double>(ns - time_frame_offset_ns_) / kNanosecondsPerSecond;
  emit playheadSeeked(seconds);
  return seconds;
}

const TimelineSpanInput* Timeline::trackById(TimelineSourceId id) const {
  for (const TimelineSpanInput& track : scene_.tracks()) {
    if (track.id == id) {
      return &track;
    }
  }
  return nullptr;
}

qint64 Timeline::baseOffsetOf(TimelineSourceId id) const {
  const TimelineSpanInput* track = trackById(id);
  return track != nullptr ? track->offset_ns : 0;
}

qint64 Timeline::offsetForBarDelta(TimelineSourceId id, double dx_px) const {
  // display = raw - offset. Dragging RIGHT (+dx) moves the bar to a LATER display
  // time, which means the offset must DECREASE: offset_new = base - delta_ns.
  const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
  return baseOffsetOf(id) - delta_ns;
}

void Timeline::setSnapEnabled(bool enabled) {
  snap_enabled_ = enabled;
  if (!enabled) {
    hideSnapLine();
  }
}

void Timeline::setInteractionLocked(bool locked) {
  if (interaction_locked_ == locked) {
    return;
  }
  interaction_locked_ = locked;
  // The widget's own Align button greys out; the host also disables its external
  // align rail (the rail drives the controller's align slots directly).
  if (align_button_ != nullptr) {
    align_button_->setEnabled(!locked);
  }
  // Show/hide the "pause to interact" overlay over the scene.
  if (lock_overlay_ != nullptr) {
    if (locked) {
      updateLockOverlayGeometry();
      lock_overlay_->show();
      lock_overlay_->raise();
    } else {
      lock_overlay_->hide();
    }
  }
  if (locked) {
    // Cancel any in-flight gesture so a drag started just before the lock can't
    // dangle (the press gates below stop NEW gestures; this clears live ones).
    drag_group_.clear();
    rubber_banding_ = false;
    if (rubber_band_ != nullptr) {
      rubber_band_->hide();
    }
    dragging_playhead_ = false;
    dragging_reference_ = false;
    dragging_pill_ = false;
    dragging_vpill_ = false;
    name_drag_index_ = -1;
    name_dragging_ = false;
    hideSnapLine();
    if (view_ != nullptr) {
      view_->viewport()->unsetCursor();
    }
  }
}

Timeline::DragSnap Timeline::computeDragSnap(qint64 raw_delta_ns) {
  if (drag_group_.empty() || viewport_.px_per_ns <= 0.0) {
    snap_active_ = false;
    return {false, raw_delta_ns, 0};
  }
  // Dragged edges come from the DRAG-START display position (raw bounds − base
  // offset). The host may have fed the mid-drag offset back into scene_ during the
  // drag, so reading scene_ for the dragged bars would double-count raw_delta and
  // make the snap target drift/flicker. Candidates are the fixed neighbours.
  std::set<TimelineSourceId> dragged;
  std::vector<qint64> dragged_edges;
  for (const DragMember& m : drag_group_) {
    dragged.insert(m.id);
    if (const TimelineSpanInput* track = trackById(m.id)) {
      dragged_edges.push_back(track->t_min_ns - m.base_offset);
      dragged_edges.push_back(track->t_max_ns - m.base_offset);
    }
  }
  std::vector<qint64> candidate_edges;
  for (const TimelineSpanInput& track : scene_.tracks()) {
    if (dragged.count(track.id) != 0) {
      continue;
    }
    const TimeSpan w = TimelineScene::displayWindow(track);
    candidate_edges.push_back(w.min);
    candidate_edges.push_back(w.max);
  }

  const auto catch_ns = static_cast<qint64>(std::llround(kSnapThresholdPx / viewport_.px_per_ns));
  // Hysteresis: keep the active snap while the cursor stays within a wider release
  // band, so it doesn't chatter between two nearby candidates. "Keep dragging to
  // unsnap" = leave that band.
  const auto release_ns =
      static_cast<qint64>(std::llround((kSnapThresholdPx + kSnapReleaseExtraPx) / viewport_.px_per_ns));
  if (snap_active_ && std::llabs(snap_active_delta_ - raw_delta_ns) <= release_ns) {
    return {true, snap_active_delta_, snap_active_edge_};
  }
  const TimelineScene::EdgeSnap snap =
      TimelineScene::snapToEdges(dragged_edges, candidate_edges, raw_delta_ns, catch_ns);
  snap_active_ = snap.snapped;
  snap_active_delta_ = snap.delta_ns;
  snap_active_edge_ = snap.edge_ns;
  return {snap.snapped, snap.delta_ns, snap.edge_ns};
}

void Timeline::showSnapLine(qint64 display_ns) {
  const double x = TimelineScene::nsToPx(display_ns, viewport_);
  snap_line_item_->setLine(x, TimelineRulerItem::kRulerHeight, x, sceneHeight());
  snap_line_item_->setVisible(true);
}

void Timeline::hideSnapLine() {
  if (snap_line_item_ != nullptr) {
    snap_line_item_->setVisible(false);
  }
}

void Timeline::repositionReference() {
  reference_item_->setPos(TimelineScene::nsToPx(reference_ns_, viewport_), 0);
}

void Timeline::moveReferenceToSceneX(double scene_x) {
  if (interaction_locked_) {
    return;  // Locked: the reference line is view-only — no user drag.
  }
  // Clamp to the data extent, same as the playhead — a reference outside the data
  // is meaningless and would otherwise be snapped back by the host's clamp.
  const TimeSpan extent = scene_.sceneExtent();
  const qint64 ns = std::clamp(TimelineScene::pxToNs(scene_x, viewport_), extent.min, extent.max);
  reference_ns_ = ns;
  repositionReference();
  reference_item_->setLabel(markerLabel(ns));
  // Emit in the host's external (playback) frame — undo the offset in integer ns.
  emit referenceLineMoved(static_cast<double>(ns - time_frame_offset_ns_) / kNanosecondsPerSecond);
}

void Timeline::setReferenceLine(double display_seconds, bool visible) {
  reference_visible_ = visible;
  reference_item_->setVisible(visible);
  if (!visible) {
    return;
  }
  reference_ns_ = static_cast<qint64>(std::llround(display_seconds * kNanosecondsPerSecond)) + time_frame_offset_ns_;
  repositionReference();
  reference_item_->setLabel(markerLabel(reference_ns_));
}

void Timeline::applyBarDragForTest(TimelineSourceId id, double dx_px) {
  if (interaction_locked_) {
    return;  // Locked: bar-offset editing is suppressed (matches the real drag gate).
  }
  emit offsetChangeRequested(id, offsetForBarDelta(id, dx_px));
}

double Timeline::seekToSceneXForTest(double scene_x) {
  // Pure slave: seekToSceneX no longer moves playhead_ns_, it returns the clamped
  // display-seconds it emitted. That emitted (clamped) value is what the test pins.
  return seekToSceneX(scene_x);
}

double Timeline::moveReferenceToSceneXForTest(double scene_x) {
  moveReferenceToSceneX(scene_x);
  return static_cast<double>(reference_ns_) / kNanosecondsPerSecond;
}

TimeSpan Timeline::sceneExtentForTest() const {
  return scene_.sceneExtent();
}

qint64 Timeline::playheadNsForTest() const {
  return playhead_ns_;
}

QString Timeline::markerLabelForTest(qint64 ns) const {
  return markerLabel(ns);
}

void Timeline::pressViewportForTest(QPoint viewport_pos) {
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  QMouseEvent ev(
      QEvent::MouseButtonPress, QPointF(viewport_pos), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  mousePressEvent(&ev);
}

void Timeline::moveViewportForTest(QPoint viewport_pos, bool button_down) {
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  const Qt::MouseButtons buttons = button_down ? Qt::LeftButton : Qt::NoButton;
  QMouseEvent ev(QEvent::MouseMove, QPointF(viewport_pos), global, Qt::NoButton, buttons, Qt::NoModifier);
  mouseMoveEvent(&ev);
}

void Timeline::releaseViewportForTest(QPoint viewport_pos) {
  const QPointF global = view_->viewport()->mapToGlobal(viewport_pos);
  QMouseEvent ev(
      QEvent::MouseButtonRelease, QPointF(viewport_pos), global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  mouseReleaseEvent(&ev);
}

bool Timeline::isScrollPillShownForTest() const {
  return pill_shown_;
}

int Timeline::scrollValueForTest() const {
  return view_->horizontalScrollBar()->value();
}

bool Timeline::horizontalScrollBarVisibleForTest() const {
  return view_->horizontalScrollBar()->isVisible();
}

int Timeline::viewportHeightForTest() const {
  return view_->viewport()->height();
}

int Timeline::viewportWidthForTest() const {
  return view_->viewport()->width();
}

bool Timeline::isVScrollPillShownForTest() const {
  return vpill_shown_;
}

bool Timeline::verticalScrollBarVisibleForTest() const {
  return view_->verticalScrollBar()->isVisible();
}

int Timeline::verticalScrollValueForTest() const {
  return view_->verticalScrollBar()->value();
}

void Timeline::setVerticalScrollForTest(int value) {
  view_->verticalScrollBar()->setValue(value);
}

double Timeline::rulerSceneYForTest() const {
  return ruler_item_ != nullptr ? ruler_item_->y() : 0.0;
}

int Timeline::nameRowCountForTest() const {
  return name_panel_->rowCountForTest();
}

double Timeline::nameRowTopForTest(int index) const {
  return name_panel_->rowTopForTest(index);
}

double Timeline::barRowTopForTest(int index) const {
  return bar_items_.at(static_cast<std::size_t>(index))->pos().y();
}

QList<quint64> Timeline::trackOrderForTest() const {
  QList<quint64> ids;
  for (const TimelineSpanInput& s : scene_.tracks()) {
    ids.push_back(s.id);
  }
  return ids;
}

void Timeline::reorderForTest(int from, int drop_index) {
  applyTrackReorder(from, drop_index);
}

bool Timeline::beginSyntheticDrag(TimelineSourceId id) {
  drag_group_.clear();
  snap_active_ = false;  // start the synthetic drag with no sticky snap held
  for (TimelineBarItem* bar : bar_items_) {
    if (bar->sourceId() == id) {
      drag_group_.push_back(DragMember{.item = bar, .id = id, .base_offset = baseOffsetOf(id)});
      return true;
    }
  }
  return false;
}

void Timeline::dragBarForSnapTest(TimelineSourceId id, double dx_px) {
  hideSnapLine();
  if (!beginSyntheticDrag(id)) {
    return;
  }
  const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
  if (snap_enabled_) {
    const DragSnap snap = computeDragSnap(delta_ns);
    if (snap.snapped) {
      showSnapLine(snap.line_display_ns);
    } else {
      hideSnapLine();
    }
  }
  drag_group_.clear();  // leave the guide-line state; end the synthetic drag
}

bool Timeline::snapLineVisibleForTest() const {
  return snap_line_item_ != nullptr && snap_line_item_->isVisible();
}

std::vector<bool> Timeline::dragSnapSequenceForTest(TimelineSourceId id, const std::vector<double>& dxs) {
  std::vector<bool> states;
  // One continuous drag for the whole sequence (beginSyntheticDrag resets the
  // sticky snap_active_ once, so hysteresis carries across the dxs).
  if (!beginSyntheticDrag(id)) {
    return states;
  }
  states.reserve(dxs.size());
  for (const double dx : dxs) {
    const qint64 delta_ns = TimelineScene::pxDeltaToNs(dx, viewport_);
    states.push_back(snap_enabled_ && computeDragSnap(delta_ns).snapped);
  }
  drag_group_.clear();
  return states;
}

void Timeline::setSelectionFromViewportRect(const QRect& viewport_rect) {
  selected_ids_.clear();
  // A near-zero rect is a plain click on empty space → clear the selection.
  if (viewport_rect.width() >= 3 || viewport_rect.height() >= 3) {
    const QList<QGraphicsItem*> hit = view_->items(viewport_rect);
    for (QGraphicsItem* it : hit) {
      if (auto* bar = dynamic_cast<TimelineBarItem*>(it)) {
        selected_ids_.insert(bar->sourceId());
      }
    }
  }
  applySelectionHighlight();
}

void Timeline::applySelectionHighlight() {
  for (TimelineBarItem* bar : bar_items_) {
    bar->setHighlighted(selected_ids_.count(bar->sourceId()) != 0);
  }
}

QList<quint64> Timeline::selectedIdsList() const {
  QList<quint64> ids;
  ids.reserve(static_cast<qsizetype>(selected_ids_.size()));
  for (const TimelineSourceId id : selected_ids_) {
    ids.push_back(id);
  }
  return ids;
}

void Timeline::showContextMenu(const QPoint& global_pos) {
  if (interaction_locked_) {
    return;  // Locked: no Merge (the only manipulation the menu offers).
  }
  QMenu menu(this);
  QAction* merge = menu.addAction(tr("Merge"));
  // Enabled only with a selection; emits the merge intent the host wires up.
  merge->setEnabled(!selected_ids_.empty());

  if (menu.exec(global_pos) == merge) {
    emit mergeRequested(selectedIdsList());
  }
}

void Timeline::wheelEvent(QWheelEvent* event) {
  // Locked (e.g. live streaming + playing): the view is frozen at the live edge —
  // no wheel scroll and no Ctrl+wheel zoom. Swallow the event so it can't navigate.
  if (interaction_locked_) {
    event->accept();
    return;
  }
  const int angle = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
  if (angle == 0) {
    QWidget::wheelEvent(event);
    return;
  }

  if (event->modifiers().testFlag(Qt::ControlModifier)) {
    // Ctrl+wheel: cursor-anchored zoom. The origin stays pinned to the scene's
    // left edge (rebuild owns it); we keep the time under the cursor fixed by
    // adjusting the horizontal scrollbar after the rebuild.
    const double factor = angle > 0 ? kZoomInFactor : kZoomOutFactor;
    const double anchor_vp_x = event->position().x();
    const double anchor_scene_x = view_->mapToScene(event->position().toPoint()).x();
    const qint64 anchor_ns = TimelineScene::pxToNs(anchor_scene_x, viewport_);
    // zoom() only scales + clamps px_per_ns here; its origin output is ignored.
    viewport_.px_per_ns = TimelineScene::zoom(viewport_, factor, anchor_scene_x).px_per_ns;
    rebuild();
    const double new_anchor_scene_x = TimelineScene::nsToPx(anchor_ns, viewport_);
    view_->horizontalScrollBar()->setValue(static_cast<int>(std::llround(new_anchor_scene_x - anchor_vp_x)));
    event->accept();
    return;
  }

  // Plain wheel: scroll the horizontal timeline (wheel-up scrolls left/earlier).
  QScrollBar* hbar = view_->horizontalScrollBar();
  hbar->setValue(hbar->value() - angle);
  event->accept();
}

void Timeline::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());

  // Locked (e.g. live streaming + playing): the whole view is frozen — no scroll-pill
  // grab, needle seek, bar-offset drag, or rubber-band selection. Scrolling/zooming is
  // also blocked (see wheelEvent). The needles still track playback via setPlayhead.
  if (interaction_locked_) {
    event->accept();
    return;
  }

  // Scroll pill: a press in the bottom strip grabs the pill. Off the handle, jump
  // it to center under the cursor first, then drag from there.
  QScrollBar* hbar = view_->horizontalScrollBar();
  if (pointInPillArea(view_pos) && hbar->maximum() > hbar->minimum()) {
    const double track_w = static_cast<double>(scroll_pill_->width());
    const double total = static_cast<double>(hbar->maximum() - hbar->minimum() + hbar->pageStep());
    const bool on_handle = view_pos.x() >= pill_x_ && view_pos.x() <= pill_x_ + pill_w_;
    if (!on_handle && track_w > 0.0) {
      const double want_left = std::clamp(view_pos.x() - (pill_w_ / 2.0), 0.0, std::max(0.0, track_w - pill_w_));
      hbar->setValue(hbar->minimum() + static_cast<int>(std::llround((want_left / track_w) * total)));
    }
    dragging_pill_ = true;
    pill_drag_start_x_ = event->globalPosition().x();
    pill_drag_start_value_ = hbar->value();
    setScrollPillShown(true);
    event->accept();
    return;
  }

  // Vertical scroll pill: same logic on the right strip / vertical scrollbar.
  QScrollBar* vbar = view_->verticalScrollBar();
  if (pointInVPillArea(view_pos) && vbar->maximum() > vbar->minimum()) {
    const double track_h = static_cast<double>(vscroll_pill_->height());
    const double total = static_cast<double>(vbar->maximum() - vbar->minimum() + vbar->pageStep());
    const bool on_handle = view_pos.y() >= vpill_y_ && view_pos.y() <= vpill_y_ + vpill_h_;
    if (!on_handle && track_h > 0.0) {
      const double want_top = std::clamp(view_pos.y() - (vpill_h_ / 2.0), 0.0, std::max(0.0, track_h - vpill_h_));
      vbar->setValue(vbar->minimum() + static_cast<int>(std::llround((want_top / track_h) * total)));
    }
    dragging_vpill_ = true;
    vpill_drag_start_y_ = event->globalPosition().y();
    vpill_drag_start_value_ = vbar->value();
    setVScrollPillShown(true);
    event->accept();
    return;
  }

  QGraphicsItem* item = view_->itemAt(view_pos);
  const QPointF scene_pos = view_->mapToScene(view_pos);

  // Grabbing the (visible) reference needle: drag it to reposition the reference.
  if (reference_visible_ && item == reference_item_) {
    dragging_reference_ = true;
    reference_item_->setGrabbed(true);  // dark-blue needle + current-time pill
    moveReferenceToSceneX(scene_pos.x());
    event->accept();
    return;
  }

  // Playhead handle or a click within the ruler band: start a seek drag.
  if (item == playhead_item_ || (item == ruler_item_ && scene_pos.y() < TimelineRulerItem::kRulerHeight)) {
    dragging_playhead_ = true;
    playhead_item_->setGrabbed(true);  // dark-purple needle + current-time pill
    seekToSceneX(scene_pos.x());
    event->accept();
    return;
  }

  // Bar drag. If the pressed bar is part of the current selection, drag the
  // whole selection together (group move); otherwise drag just this bar.
  if (auto* bar = dynamic_cast<TimelineBarItem*>(item)) {
    const TimelineSourceId pressed_id = bar->sourceId();
    const bool group_move = selected_ids_.count(pressed_id) != 0;
    drag_group_.clear();
    snap_active_ = false;  // fresh drag: start with no sticky snap held
    drag_start_scene_x_ = scene_pos.x();
    for (TimelineBarItem* candidate : bar_items_) {
      const TimelineSourceId cid = candidate->sourceId();
      const bool in_drag = group_move ? (selected_ids_.count(cid) != 0) : (cid == pressed_id);
      if (!in_drag) {
        continue;
      }
      drag_group_.push_back(DragMember{.item = candidate, .id = cid, .base_offset = baseOffsetOf(cid)});
    }
    view_->viewport()->setCursor(Qt::ClosedHandCursor);
    event->accept();
    return;
  }

  // Empty space: begin a rubber-band multi-selection (clears any prior one).
  rubber_banding_ = true;
  rubber_origin_vp_ = view_pos;
  if (rubber_band_ == nullptr) {
    rubber_band_ = new timeline_detail::TimelineRubberBand(view_->viewport());
  }
  rubber_band_->setGeometry(QRect(rubber_origin_vp_, QSize()));
  rubber_band_->show();
  selected_ids_.clear();
  applySelectionHighlight();
  event->accept();
}

void Timeline::mouseMoveEvent(QMouseEvent* event) {
  const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());
  const QPointF scene_pos = view_->mapToScene(view_pos);

  // Scroll pill drag: map the global-x delta since grab to a scrollbar value delta.
  if (dragging_pill_) {
    QScrollBar* hbar = view_->horizontalScrollBar();
    const double track_w = static_cast<double>(scroll_pill_->width());
    const double total = static_cast<double>(hbar->maximum() - hbar->minimum() + hbar->pageStep());
    if (track_w > 0.0) {
      const double dx = event->globalPosition().x() - pill_drag_start_x_;
      hbar->setValue(pill_drag_start_value_ + static_cast<int>(std::llround((dx / track_w) * total)));
    }
    event->accept();
    return;
  }
  // Vertical scroll pill drag: same, mapping the global-y delta to the vbar.
  if (dragging_vpill_) {
    QScrollBar* vbar = view_->verticalScrollBar();
    const double track_h = static_cast<double>(vscroll_pill_->height());
    const double total = static_cast<double>(vbar->maximum() - vbar->minimum() + vbar->pageStep());
    if (track_h > 0.0) {
      const double dy = event->globalPosition().y() - vpill_drag_start_y_;
      vbar->setValue(vpill_drag_start_value_ + static_cast<int>(std::llround((dy / track_h) * total)));
    }
    event->accept();
    return;
  }

  if (rubber_banding_) {
    rubber_band_->setGeometry(QRect(rubber_origin_vp_, view_pos).normalized());
    // Live: highlight/unhighlight bars as the band sweeps over them.
    setSelectionFromViewportRect(rubber_band_->geometry());
    event->accept();
    return;
  }
  if (dragging_reference_) {
    moveReferenceToSceneX(scene_pos.x());
    event->accept();
    return;
  }
  if (dragging_playhead_) {
    seekToSceneX(scene_pos.x());
    event->accept();
    return;
  }
  if (!drag_group_.empty()) {
    const double dx_px = scene_pos.x() - drag_start_scene_x_;
    qint64 delta_ns = TimelineScene::pxDeltaToNs(dx_px, viewport_);
    double applied_dx_px = dx_px;
    // Edge-snap: align a dragged start/end onto a neighbour's start/end when within
    // the threshold; a guide line marks it. Dragging past the threshold releases it.
    if (snap_enabled_) {
      const DragSnap snap = computeDragSnap(delta_ns);
      if (snap.snapped) {
        delta_ns = snap.delta_ns;
        applied_dx_px = static_cast<double>(delta_ns) * viewport_.px_per_ns;  // ghost follows the snap
        showSnapLine(snap.line_display_ns);
      } else {
        hideSnapLine();
      }
    }
    // Every group member shifts by the SAME (possibly snapped) delta (rigid move).
    for (const DragMember& m : drag_group_) {
      m.item->setGhostDx(applied_dx_px);
      // Live intent per member; the host applies each and feeds state back.
      emit offsetChangeRequested(m.id, m.base_offset - delta_ns);
    }
    // Keep the ruler tint AND the empty-area hatch tracking the moving group in
    // real time (a full rebuild is suppressed mid-drag, so update them directly).
    updateDataSpanFromItems();
    event->accept();
    return;
  }
  // No drag in progress: reveal each pill while the cursor is in its strip (bottom
  // for horizontal, right for vertical), hide otherwise. (Skipped during a drag.)
  setScrollPillShown(pointInPillArea(view_pos));
  setVScrollPillShown(pointInVPillArea(view_pos));
  QWidget::mouseMoveEvent(event);
}

void Timeline::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (dragging_pill_) {
    dragging_pill_ = false;
    // Fade out if the cursor ended outside the strip; otherwise stay shown until it leaves.
    const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());
    setScrollPillShown(pointInPillArea(view_pos));
    event->accept();
    return;
  }
  if (dragging_vpill_) {
    dragging_vpill_ = false;
    const QPoint view_pos = view_->viewport()->mapFromGlobal(event->globalPosition().toPoint());
    setVScrollPillShown(pointInVPillArea(view_pos));
    event->accept();
    return;
  }
  if (rubber_banding_) {
    rubber_banding_ = false;
    const QRect rb = rubber_band_->geometry();
    rubber_band_->hide();
    setSelectionFromViewportRect(rb);
    event->accept();
    return;
  }
  if (dragging_reference_) {
    dragging_reference_ = false;
    reference_item_->setGrabbed(false);  // back to the idle light-blue needle
    event->accept();
    return;
  }
  if (dragging_playhead_) {
    dragging_playhead_ = false;
    playhead_item_->setGrabbed(false);  // back to the idle pink needle, pill hidden
    event->accept();
    return;
  }
  if (!drag_group_.empty()) {
    for (const DragMember& m : drag_group_) {
      m.item->setGhostDx(0.0);
    }
    drag_group_.clear();
    hideSnapLine();
    view_->viewport()->unsetCursor();
    // A rebuild was suppressed during the drag; re-lay-out now from whatever the
    // host wrote back (or the original state if it ignored the intent).
    rebuild();
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void Timeline::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  rebuild();
  updateScrollPillGeometry();  // re-overlay the bottom strip at the new size
}

void Timeline::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::StyleChange) {
    // Items read QGuiApplication::palette() each paint; refresh the view's own
    // backing brush and force a full repaint so the theme switch is immediate.
    view_->setBackgroundBrush(QGuiApplication::palette().color(QPalette::Window));
    gscene_->update();
    view_->viewport()->update();
    if (name_panel_ != nullptr) {
      name_panel_->update();  // re-reads QGuiApplication::palette() on repaint
    }
  }
}

bool Timeline::eventFilter(QObject* watched, QEvent* event) {
  if (watched == name_panel_) {
    switch (event->type()) {
      case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (me->button() == Qt::LeftButton) {
          namePanelPress(me->position().toPoint());
          return true;
        }
        break;
      }
      case QEvent::MouseMove:
        namePanelMove(
            static_cast<QMouseEvent*>(event)->position().toPoint(),
            (static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton) != 0);
        return name_dragging_;
      case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
          namePanelRelease(static_cast<QMouseEvent*>(event)->position().toPoint());
          return true;
        }
        break;
      default:
        break;
    }
    return QWidget::eventFilter(watched, event);
  }
  if (watched == view_->viewport()) {
    switch (event->type()) {
      case QEvent::MouseButtonPress:
        mousePressEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::MouseMove:
        mouseMoveEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::MouseButtonRelease:
        mouseReleaseEvent(static_cast<QMouseEvent*>(event));
        return event->isAccepted();
      case QEvent::Wheel:
        wheelEvent(static_cast<QWheelEvent*>(event));
        return event->isAccepted();
      case QEvent::ContextMenu:
        showContextMenu(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
      case QEvent::Leave:
        // Cursor left the view: fade the scroll pills out (unless a drag holds one).
        if (!dragging_pill_) {
          setScrollPillShown(false);
        }
        if (!dragging_vpill_) {
          setVScrollPillShown(false);
        }
        break;
      default:
        break;
    }
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace PJ
