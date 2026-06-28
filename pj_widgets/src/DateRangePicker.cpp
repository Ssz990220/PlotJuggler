// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Date/time range picker bundle, wrapped in namespace PJ. Theming is driven from
// the persisted-theme tokens (pickerTokens); the month-nav arrows use the app's
// Material "keyboard_arrow_left/right" chevron SVGs (recolored to the theme ink,
// or the from/to hint colors) so they read big and match the rest of the chrome.
// See DateRangePicker.h.

#include <pj_widgets/DateRangePicker.h>

#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSpinBox>
#include <QString>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <algorithm>

namespace PJ {

namespace {

constexpr int kRows = 6;
constexpr int kCols = 7;
constexpr int kHeaderPad = 8;

// Month-nav chevron icon size — deliberately large so the arrows read as the same
// chevrons used elsewhere in the chrome, not thin punctuation glyphs.
constexpr int kNavChevronPx = 24;

const QColor kFromHintColor(0x2e, 0xcc, 0x71);  // green
const QColor kToHintColor(0xe7, 0x4c, 0x3c);    // red

// The app persists its active theme to QSettings("StyleSheet::theme")
// (Theme.cpp) — the same key the rest of pj_dialog_host (widget_binding.cpp)
// reads. We key off it because the calendar paints from palette() roles, but
// the app pins QPalette at Fusion defaults regardless of the active QSS theme,
// so palette() resolves to Fusion light-grey on both themes. Sourcing colors
// from theme tokens chosen by this key is the fix.
bool pickerThemeIsLight() {
  return QSettings().value(QStringLiteral("StyleSheet::theme"), QStringLiteral("light")).toString().contains("light");
}

// Theme-token color set for the calendar/overlay. Values mirror the app's QSS
// tokens (light / dark) so the picker reads correctly on both themes. The
// semantic accents (weekend red, from-green, to-red, white-on-endpoint) stay
// hardcoded at their call sites — they are theme-agnostic by design.
struct PickerTokens {
  QColor surface;          // calendar body + overlay background
  QColor text;             // month/year title, day numbers
  QColor muted;            // weekday header row, grid lines
  QColor border;           // overlay frame border, grid
  QColor selection_range;  // in-range fill (used at alpha ~60)
  QColor hover_fill;       // hover cell fill
  QColor hover_border;     // hover cell border (== border)
};

PickerTokens pickerTokens() {
  PickerTokens t;
  if (pickerThemeIsLight()) {
    t.surface = QColor(0xFF, 0xFF, 0xFF);          // pure white calendar/overlay surface
    t.text = QColor(0x11, 0x11, 0x11);             // #111111  default_text
    t.muted = QColor(0x66, 0x66, 0x66);            // #666666  disabled_text
    t.border = QColor(0xc0, 0xc0, 0xc0);           // #c0c0c0  border_default
    t.selection_range = QColor(0xC2, 0xDC, 0xFF);  // #C2DCFF  item_selection_background
    t.hover_fill = QColor(0, 0, 0, 40);            // rgba(0,0,0,40) hover_overlay
  } else {
    t.surface = QColor(0x3B, 0x3B, 0x47);          // #3B3B47  dark_background
    t.text = QColor(0xF0, 0xF0, 0xF0);             // #F0F0F0  default_text
    t.muted = QColor(0x77, 0x77, 0x77);            // #777777  disabled_text
    t.border = QColor(0xB0, 0xB0, 0xBF);           // #B0B0BF  border_default
    t.selection_range = QColor(0x14, 0x8C, 0xD2);  // #148CD2  item_selection_background
    t.hover_fill = QColor(255, 255, 255, 30);      // rgba(255,255,255,30) hover_overlay
  }
  t.hover_border = t.border;
  return t;
}

// Recolor a monochrome icon SVG to the active theme's ink. Local copy of
// widget_binding.cpp::recolorSvgInk (kept local because pj_widgets/SvgUtil.h is
// not on this module's include path): swaps #3D3D3D<->#E0E0E0 so the icon
// authored for one theme renders in the other's ink, plus legacy black/white.
void recolorSvgInk(QByteArray& svg_data, bool light_theme) {
  const QByteArray ink = light_theme ? QByteArray("#3D3D3D") : QByteArray("#E0E0E0");
  const QByteArray opposite = light_theme ? QByteArray("#E0E0E0") : QByteArray("#3D3D3D");
  svg_data.replace(opposite, ink);   // palette swap (icon authored for the other theme)
  svg_data.replace("#000000", ink);  // legacy black -> ink
  svg_data.replace("#ffffff", opposite);
  const qsizetype svg_open = svg_data.indexOf("<svg");
  if (svg_open < 0) {
    return;
  }
  const qsizetype tag_end = svg_data.indexOf('>', svg_open);
  if (tag_end <= svg_open) {
    return;
  }
  // Inject a root fill only when none is present, so fill-less Material paths
  // inherit the theme ink.
  if (svg_data.mid(svg_open, tag_end - svg_open).contains("fill=\"")) {
    return;
  }
  svg_data.insert(tag_end, " fill=\"" + ink + "\"");
}

// Rasterize already-prepared SVG bytes into a px-logical QIcon at size*DPR so it
// stays crisp on HiDPI. Callers own the theming/recoloring of `svg_data`; this is
// the shared QSvgRenderer + DPR-scaling tail behind both icon paths below (and
// mirrors widget_binding.cpp's render path).
QIcon rasterizeSvgIcon(const QByteArray& svg_data, int px) {
  QSvgRenderer renderer(svg_data);
  if (!renderer.isValid()) {
    return QIcon();
  }
  const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
  QImage image(QSize(px, px) * dpr, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  QPixmap pix = QPixmap::fromImage(image);
  pix.setDevicePixelRatio(dpr);
  return QIcon(pix);
}

// Render a qrc SVG into a theme-inked QIcon.
QIcon renderThemedIcon(const QString& resource_path, int px = 16) {
  QFile file(resource_path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QIcon();
  }
  QByteArray svg_data = file.readAll();
  file.close();
  recolorSvgInk(svg_data, pickerThemeIsLight());
  return rasterizeSvgIcon(svg_data, px);
}

// Render one of the app's Material chevron SVGs (keyboard_arrow_left/right),
// recolored to `color`, into a px*DPR QIcon. The source path's own fill is
// irrelevant — we always load the "_light" variant and swap its #3D3D3D fill for
// `color` (theme ink for the resting state, the from/to hint colors when active).
// Cached by (direction, color, px): updateNavButtons() re-applies icons on every
// hovered-date change, so without the cache each mouse move would re-parse + raster
// four SVGs.
QIcon renderChevronIcon(bool left, const QColor& color, int px) {
  static QHash<QString, QIcon> cache;
  // Key on HexRgb to match the recolor below (color.name() drops alpha), so the
  // key can never disagree with the rasterized output.
  const QString key = QStringLiteral("%1:%2:%3").arg(left ? 1 : 0).arg(color.name()).arg(px);
  const auto cached = cache.constFind(key);
  if (cached != cache.constEnd()) {
    return cached.value();
  }

  const QString path = left ? QStringLiteral(":/resources/svg/keyboard_arrow_left_light.svg")
                            : QStringLiteral(":/resources/svg/keyboard_arrow_right_light.svg");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QIcon();
  }
  QByteArray svg_data = file.readAll();
  file.close();
  svg_data.replace("#3D3D3D", color.name().toUtf8());
  QIcon icon = rasterizeSvgIcon(svg_data, px);
  cache.insert(key, icon);
  return icon;
}

}  // namespace

// ===========================================================================
// CalendarWidget
// ===========================================================================

CalendarWidget::CalendarWidget(QWidget* parent)
    : QWidget(parent), year_(QDate::currentDate().year()), month_(QDate::currentDate().month()) {
  setMouseTracking(true);
  setMinimumSize(320, 260);
}

void CalendarWidget::setMonth(int year, int month) {
  year_ = year;
  month_ = month;
  update();
}

void CalendarWidget::setMediated(bool mediated) {
  mediated_ = mediated;
}

void CalendarWidget::setRange(const QDate& from, const QDate& to) {
  range_from_ = from;
  range_to_ = to;
  update();
}

void CalendarWidget::setHoverDate(const QDate& date) {
  hover_date_ = date;
  update();
}

void CalendarWidget::clearRange() {
  range_from_ = QDate();
  range_to_ = QDate();
  hover_date_ = QDate();
  state_ = State::kIdle;
  update();
}

int CalendarWidget::headerHeight() const {
  return fontMetrics().height() * 2 + kHeaderPad * 3;
}

int CalendarWidget::firstDayColumn() const {
  int dow = QDate(year_, month_, 1).dayOfWeek();  // Qt: 1=Mon, 7=Sun
  return dow - 1;
}

QRect CalendarWidget::cellRect(int row, int col) const {
  int hh = headerHeight();
  int cell_w = width() / kCols;
  int cell_h = (height() - hh) / kRows;
  return QRect(col * cell_w, hh + row * cell_h, cell_w, cell_h);
}

QDate CalendarWidget::dateAtPosition(const QPoint& pos) const {
  int hh = headerHeight();
  if (pos.y() < hh) {
    return QDate();
  }
  int cell_w = width() / kCols;
  int cell_h = (height() - hh) / kRows;
  int col = pos.x() / cell_w;
  int row = (pos.y() - hh) / cell_h;
  if (col < 0 || col >= kCols || row < 0 || row >= kRows) {
    return QDate();
  }
  int day_index = row * kCols + col - firstDayColumn();
  int day = day_index + 1;
  if (day < 1 || day > QDate(year_, month_, 1).daysInMonth()) {
    return QDate();
  }
  return QDate(year_, month_, day);
}

void CalendarWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  // Source colors from the active theme's tokens instead of palette() roles:
  // the app pins QPalette at Fusion defaults regardless of the QSS theme, so
  // palette() would resolve to Fusion light-grey on both themes. pickerTokens()
  // picks the right light/dark shades off the persisted "StyleSheet::theme" key.
  const PickerTokens tok = pickerTokens();
  p.fillRect(rect(), tok.surface);

  int hh = headerHeight();
  int cell_w = width() / kCols;
  int fm_h = fontMetrics().height();

  // Month/Year title
  QFont title_font = font();
  title_font.setBold(true);
  title_font.setPointSize(font().pointSize() + 2);
  p.setFont(title_font);
  QString title = QDate(year_, month_, 1).toString("MMMM yyyy");
  p.setPen(tok.text);
  p.drawText(QRect(0, 0, width(), fm_h + kHeaderPad * 2), Qt::AlignCenter, title);

  // Day name headers
  p.setFont(font());
  static const char* day_names[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  int day_header_y = fm_h + kHeaderPad * 2;
  QColor weekend_color(0xef, 0x53, 0x50);
  QColor header_color = tok.muted;
  for (int c = 0; c < kCols; ++c) {
    QRect r(c * cell_w, day_header_y, cell_w, fm_h + kHeaderPad);
    p.setPen((c >= 5) ? weekend_color : header_color);
    p.drawText(r, Qt::AlignCenter, day_names[c]);
  }

  // Effective range for painting
  QDate eff_from = range_from_;
  QDate eff_to = range_to_;
  if (eff_from.isValid() && !eff_to.isValid() && hover_date_.isValid()) {
    eff_to = hover_date_;
  }
  if (eff_from.isValid() && eff_to.isValid() && eff_from > eff_to) {
    std::swap(eff_from, eff_to);
  }

  int days_in_month = QDate(year_, month_, 1).daysInMonth();
  int first_col = firstDayColumn();

  QColor from_color(0x2e, 0xcc, 0x71);
  QColor to_color(0xe7, 0x4c, 0x3c);
  QColor range_color = tok.selection_range;
  range_color.setAlpha(60);
  QColor hover_brush = tok.hover_fill;
  QColor hover_border = tok.hover_border;

  for (int day = 1; day <= days_in_month; ++day) {
    int idx = (day - 1) + first_col;
    int row = idx / kCols;
    int col = idx % kCols;
    QRect cell = cellRect(row, col);
    QDate date(year_, month_, day);

    bool is_from = eff_from.isValid() && date == eff_from;
    bool is_to = eff_to.isValid() && date == eff_to;
    bool in_range = eff_from.isValid() && eff_to.isValid() && date > eff_from && date < eff_to;
    bool is_hovered = hover_date_.isValid() && date == hover_date_ && !is_from && !is_to && !in_range;

    QRect inner = cell.adjusted(1, 1, -1, -1);
    if (is_from) {
      p.setBrush(from_color);
      p.setPen(Qt::NoPen);
      p.drawRoundedRect(inner, 6, 6);
    } else if (is_to) {
      p.setBrush(to_color);
      p.setPen(Qt::NoPen);
      p.drawRoundedRect(inner, 6, 6);
    } else if (in_range) {
      p.setBrush(range_color);
      p.setPen(Qt::NoPen);
      p.drawRect(inner);
    } else if (is_hovered) {
      p.setBrush(hover_brush);
      p.setPen(QPen(hover_border, 1));
      p.drawRoundedRect(inner, 4, 4);
    }

    QColor text_color;
    if (is_from || is_to) {
      text_color = Qt::white;
    } else if (col >= 5) {
      text_color = weekend_color;
    } else {
      text_color = tok.text;
    }
    p.setPen(text_color);
    p.setFont(font());
    p.drawText(cell, Qt::AlignCenter, QString::number(day));
  }

  QColor grid_color = tok.border;
  grid_color.setAlpha(80);
  p.setPen(QPen(grid_color, 0.5));
  for (int r = 0; r <= kRows; ++r) {
    QRect cell = cellRect(r, 0);
    p.drawLine(0, cell.y(), width(), cell.y());
  }
  for (int c = 0; c <= kCols; ++c) {
    int x = c * cell_w;
    p.drawLine(x, hh, x, height());
  }
}

void CalendarWidget::mouseMoveEvent(QMouseEvent* event) {
  QDate date = dateAtPosition(event->pos());
  setCursor(date.isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
  if (date.isValid()) {
    if (mediated_) {
      emit dateHovered(date);
    } else if (state_ == State::kSelecting) {
      hover_date_ = date;
      update();
    } else if (hover_date_ != date) {
      hover_date_ = date;
      update();
    }
  } else {
    if (mediated_) {
      emit hoverLeft();
    } else if (hover_date_.isValid()) {
      hover_date_ = QDate();
      update();
    }
  }
}

void CalendarWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  QDate date = dateAtPosition(event->pos());
  if (!date.isValid()) {
    return;
  }
  if (mediated_) {
    emit dateClicked(date);
    return;
  }
  switch (state_) {
    case State::kIdle:
      range_from_ = date;
      range_to_ = QDate();
      state_ = State::kSelecting;
      break;
    case State::kSelecting:
      range_to_ = date;
      if (range_from_ > range_to_) {
        std::swap(range_from_, range_to_);
      }
      hover_date_ = QDate();
      state_ = State::kCommitted;
      break;
    case State::kCommitted:
      if (qAbs(range_from_.daysTo(date)) <= qAbs(range_to_.daysTo(date))) {
        range_from_ = date;
      } else {
        range_to_ = date;
      }
      if (range_from_ > range_to_) {
        std::swap(range_from_, range_to_);
      }
      break;
  }
  update();
}

void CalendarWidget::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    return;
  }
  QDate date = dateAtPosition(event->pos());
  if (!date.isValid()) {
    return;
  }
  if (mediated_) {
    emit dateDoubleClicked(date);
    return;
  }
  range_from_ = date;
  range_to_ = date;
  hover_date_ = QDate();
  state_ = State::kCommitted;
  update();
}

void CalendarWidget::leaveEvent(QEvent* event) {
  Q_UNUSED(event);
  if (mediated_) {
    emit hoverLeft();
  } else if (hover_date_.isValid()) {
    hover_date_ = QDate();
    update();
  }
}

// ===========================================================================
// TimePickerWidget
// ===========================================================================

TimePickerWidget::TimePickerWidget(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);

  auto make_time_group = [&](QLabel*& date_label, QSpinBox*& hour, QSpinBox*& minute, QComboBox*& ampm) {
    date_label = new QLabel("---");
    hour = new QSpinBox;
    hour->setRange(1, 12);
    hour->setWrapping(true);
    // The app QSS adds ~28px of horizontal padding to spin boxes and combos,
    // which would squeeze the value behind the arrows at narrower sizes.
    // These widths keep the controls tidy and aligned. The HEIGHT is driven from
    // the app QSS (#PickerOverlay QSpinBox/QComboBox → 26px), which overrides the
    // global 18px input cap so the time row matches the dialog's other dropdowns.
    hour->setFixedWidth(56);
    hour->setAlignment(Qt::AlignCenter);
    auto* colon = new QLabel(":");
    minute = new QSpinBox;
    minute->setRange(0, 59);
    minute->setWrapping(true);
    minute->setFixedWidth(56);
    minute->setAlignment(Qt::AlignCenter);
    minute->setSpecialValueText("00");
    ampm = new QComboBox;
    ampm->addItems({"AM", "PM"});
    // Wider than the 56px spin boxes: a combo adds the ~18px drop-down arrow on
    // top of the app QSS's ~28px horizontal padding, so 64px clipped "AM"/"PM".
    ampm->setFixedWidth(76);
    layout->addWidget(date_label);
    layout->addWidget(hour);
    layout->addWidget(colon);
    layout->addWidget(minute);
    layout->addWidget(ampm);
  };

  make_time_group(from_date_label_, from_hour_, from_minute_, from_am_pm_);
  from_hour_->setValue(12);
  from_minute_->setValue(0);
  from_am_pm_->setCurrentIndex(0);

  layout->addSpacing(16);

  make_time_group(to_date_label_, to_hour_, to_minute_, to_am_pm_);
  to_hour_->setValue(11);
  to_minute_->setValue(59);
  to_am_pm_->setCurrentIndex(1);

  layout->addStretch();

  auto emit_changed = [this]() { emit timeChanged(); };
  connect(from_hour_, QOverload<int>::of(&QSpinBox::valueChanged), this, emit_changed);
  connect(from_minute_, QOverload<int>::of(&QSpinBox::valueChanged), this, emit_changed);
  connect(from_am_pm_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, emit_changed);
  connect(to_hour_, QOverload<int>::of(&QSpinBox::valueChanged), this, emit_changed);
  connect(to_minute_, QOverload<int>::of(&QSpinBox::valueChanged), this, emit_changed);
  connect(to_am_pm_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, emit_changed);
}

void TimePickerWidget::setFromDate(const QDate& date) {
  from_date_label_->setText(date.isValid() ? date.toString("ddd dd-MM-yy") : QStringLiteral("---"));
}

void TimePickerWidget::setToDate(const QDate& date) {
  to_date_label_->setText(date.isValid() ? date.toString("ddd dd-MM-yy") : QStringLiteral("---"));
}

QTime TimePickerWidget::timeFrom12Hour(int hour12, int minute, const QString& ampm) const {
  int h = hour12 % 12;
  if (ampm == "PM") {
    h += 12;
  }
  return QTime(h, minute);
}

QTime TimePickerWidget::fromTime() const {
  return timeFrom12Hour(from_hour_->value(), from_minute_->value(), from_am_pm_->currentText());
}

QTime TimePickerWidget::toTime() const {
  return timeFrom12Hour(to_hour_->value(), to_minute_->value(), to_am_pm_->currentText());
}

// ===========================================================================
// DualCalendarWidget
// ===========================================================================

namespace {
// Signed month comparison. Returns 0 for an invalid date.
int monthCmp(const QDate& d, int year, int month) {
  if (!d.isValid()) {
    return 0;
  }
  if (d.year() != year) {
    return (d.year() < year) ? -1 : +1;
  }
  if (d.month() != month) {
    return (d.month() < month) ? -1 : +1;
  }
  return 0;
}
}  // namespace

void DualCalendarWidget::advanceMonth(int& year, int& month, int delta) {
  month += delta;
  while (month > 12) {
    month -= 12;
    year++;
  }
  while (month < 1) {
    month += 12;
    year--;
  }
}

DualCalendarWidget::DualCalendarWidget(QWidget* parent)
    : QWidget(parent),
      left_prev_(new QPushButton),
      left_next_(new QPushButton),
      right_prev_(new QPushButton),
      right_next_(new QPushButton),
      left_year_(QDate::currentDate().year()),
      left_month_(QDate::currentDate().month()),
      left_max_year_(QDate::currentDate().year()),
      left_max_month_(QDate::currentDate().month()) {
  right_year_ = left_year_;
  right_month_ = left_month_;
  advanceMonth(right_year_, right_month_, 1);
  right_min_year_ = right_year_;
  right_min_month_ = right_month_;

  // Flat icon buttons carrying the big themed chevrons (28x28 to match the rest of
  // the chrome's icon buttons); updateNavButtons() re-inks them per hint state.
  const QSize btn_size(28, 28);
  const QColor ink = pickerTokens().text;
  auto setup_nav = [&](QPushButton* b, bool left) {
    b->setIcon(renderChevronIcon(left, ink, kNavChevronPx));
    b->setIconSize(QSize(kNavChevronPx, kNavChevronPx));
    b->setFixedSize(btn_size);
    b->setFlat(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton { border: none; background: transparent; padding: 0; }"));
  };
  setup_nav(left_prev_, /*left=*/true);
  setup_nav(right_prev_, /*left=*/true);
  setup_nav(left_next_, /*left=*/false);
  setup_nav(right_next_, /*left=*/false);

  auto* left_nav = new QHBoxLayout;
  left_nav->addWidget(left_prev_);
  left_nav->addStretch();
  left_nav->addWidget(left_next_);
  auto* left_col = new QVBoxLayout;
  left_col->addLayout(left_nav);
  auto* left_cal = new CalendarWidget;
  left_cal->setMediated(true);
  calendars_.append(left_cal);
  left_col->addWidget(left_cal);
  connect(left_prev_, &QPushButton::clicked, this, &DualCalendarWidget::leftPrev);
  connect(left_next_, &QPushButton::clicked, this, &DualCalendarWidget::leftNext);

  auto* right_nav = new QHBoxLayout;
  right_nav->addWidget(right_prev_);
  right_nav->addStretch();
  right_nav->addWidget(right_next_);
  auto* right_col = new QVBoxLayout;
  right_col->addLayout(right_nav);
  auto* right_cal = new CalendarWidget;
  right_cal->setMediated(true);
  calendars_.append(right_cal);
  right_col->addWidget(right_cal);
  connect(right_prev_, &QPushButton::clicked, this, &DualCalendarWidget::rightPrev);
  connect(right_next_, &QPushButton::clicked, this, &DualCalendarWidget::rightNext);

  for (auto* cal : calendars_) {
    connect(cal, &CalendarWidget::dateClicked, this, &DualCalendarWidget::onDateClicked);
    connect(cal, &CalendarWidget::dateHovered, this, &DualCalendarWidget::onDateHovered);
    connect(cal, &CalendarWidget::hoverLeft, this, &DualCalendarWidget::onHoverLeft);
  }

  auto* main_layout = new QHBoxLayout(this);
  main_layout->setContentsMargins(8, 8, 8, 8);
  main_layout->setSpacing(16);
  main_layout->addLayout(left_col);
  main_layout->addLayout(right_col);

  updateCalendars();
}

void DualCalendarWidget::setExternalRange(const QDate& from, const QDate& to) {
  const QDate today = QDate::currentDate();
  left_max_year_ = today.year();
  left_max_month_ = today.month();
  right_min_year_ = left_max_year_;
  right_min_month_ = left_max_month_;
  advanceMonth(right_min_year_, right_min_month_, 1);

  int new_left_year = left_year_;
  int new_left_month = left_month_;
  int new_right_year = right_year_;
  int new_right_month = right_month_;

  if (from.isValid()) {
    new_left_year = from.year();
    new_left_month = from.month();
    const bool to_strictly_later =
        to.isValid() && (to.year() > new_left_year || (to.year() == new_left_year && to.month() > new_left_month));
    if (to_strictly_later) {
      new_right_year = to.year();
      new_right_month = to.month();
    }
  } else if (to.isValid()) {
    new_right_year = to.year();
    new_right_month = to.month();
    new_left_year = new_right_year;
    new_left_month = new_right_month;
    advanceMonth(new_left_year, new_left_month, -1);
  }

  auto month_before = [](int y1, int m1, int y2, int m2) { return y1 < y2 || (y1 == y2 && m1 < m2); };
  if (month_before(left_max_year_, left_max_month_, new_left_year, new_left_month)) {
    new_left_year = left_max_year_;
    new_left_month = left_max_month_;
  }
  int left_plus_one_y = new_left_year;
  int left_plus_one_m = new_left_month;
  advanceMonth(left_plus_one_y, left_plus_one_m, 1);
  if (month_before(new_right_year, new_right_month, left_plus_one_y, left_plus_one_m)) {
    new_right_year = left_plus_one_y;
    new_right_month = left_plus_one_m;
  }

  QDate new_from = from.isValid() ? from : QDate();
  QDate new_to = to.isValid() ? to : QDate();

  if (new_left_year == left_year_ && new_left_month == left_month_ && new_right_year == right_year_ &&
      new_right_month == right_month_ && new_from == range_from_ && new_to == range_to_ && !selecting_) {
    return;
  }

  left_year_ = new_left_year;
  left_month_ = new_left_month;
  right_year_ = new_right_year;
  right_month_ = new_right_month;
  range_from_ = new_from;
  range_to_ = new_to;
  hover_date_ = QDate();
  selecting_ = false;
  updateCalendars();
}

void DualCalendarWidget::retheme() {
  updateNavButtons();  // re-inks the chevrons via renderChevronIcon(pickerTokens().text)
  for (auto* cal : calendars_) {
    cal->update();
  }
}

void DualCalendarWidget::leftPrev() {
  advanceMonth(left_year_, left_month_, -1);
  updateCalendars();
}

void DualCalendarWidget::leftNext() {
  int cand_year = left_year_, cand_month = left_month_;
  advanceMonth(cand_year, cand_month, 1);
  if (cand_year > left_max_year_ || (cand_year == left_max_year_ && cand_month > left_max_month_)) {
    return;
  }
  if (cand_year > right_year_ || (cand_year == right_year_ && cand_month >= right_month_)) {
    return;
  }
  left_year_ = cand_year;
  left_month_ = cand_month;
  updateCalendars();
}

void DualCalendarWidget::rightPrev() {
  int cand_year = right_year_, cand_month = right_month_;
  advanceMonth(cand_year, cand_month, -1);
  if (cand_year < left_year_ || (cand_year == left_year_ && cand_month <= left_month_)) {
    return;
  }
  const bool currently_ge_min =
      right_year_ > right_min_year_ || (right_year_ == right_min_year_ && right_month_ >= right_min_month_);
  if (currently_ge_min &&
      (cand_year < right_min_year_ || (cand_year == right_min_year_ && cand_month < right_min_month_))) {
    return;
  }
  right_year_ = cand_year;
  right_month_ = cand_month;
  updateCalendars();
}

void DualCalendarWidget::rightNext() {
  advanceMonth(right_year_, right_month_, 1);
  updateCalendars();
}

void DualCalendarWidget::updateCalendars() {
  calendars_[0]->setMonth(left_year_, left_month_);
  calendars_[1]->setMonth(right_year_, right_month_);
  updateNavButtons();
  broadcastState();
}

void DualCalendarWidget::updateNavButtons() {
  int ln_y = left_year_, ln_m = left_month_;
  advanceMonth(ln_y, ln_m, 1);
  bool at_max = ln_y > left_max_year_ || (ln_y == left_max_year_ && ln_m > left_max_month_);
  bool at_right = ln_y > right_year_ || (ln_y == right_year_ && ln_m >= right_month_);
  left_next_->setEnabled(!at_max && !at_right);

  int rp_y = right_year_, rp_m = right_month_;
  advanceMonth(rp_y, rp_m, -1);
  const bool currently_ge_min =
      right_year_ > right_min_year_ || (right_year_ == right_min_year_ && right_month_ >= right_min_month_);
  bool at_minimum =
      currently_ge_min && (rp_y < right_min_year_ || (rp_y == right_min_year_ && rp_m < right_min_month_));
  bool at_left = rp_y < left_year_ || (rp_y == left_year_ && rp_m <= left_month_);
  right_prev_->setEnabled(!at_minimum && !at_left);

  left_prev_->setEnabled(true);
  right_next_->setEnabled(true);

  // Direction hint: color the arrow text green/red when clicking it would
  // reveal the start/end date (currently outside both calendars' views).
  const int from_l = monthCmp(range_from_, left_year_, left_month_);
  const int from_r = monthCmp(range_from_, right_year_, right_month_);
  const int to_l = monthCmp(range_to_, left_year_, left_month_);
  const int to_r = monthCmp(range_to_, right_year_, right_month_);

  const bool from_before_l = from_l < 0;
  const bool from_in_gap = from_l > 0 && from_r < 0;
  const bool from_after_r = from_r > 0;
  const bool to_before_l = to_l < 0;
  const bool to_in_gap = to_l > 0 && to_r < 0;
  const bool to_after_r = to_r > 0;

  // Direction hint: recolor the chevron green/red when clicking it would reveal the
  // start/end date (currently outside both calendars' views); resting state is the
  // theme ink. The flat-button stylesheet set in the ctor is left untouched.
  const QColor ink = pickerTokens().text;
  auto apply_hint = [ink](QPushButton* btn, bool left, bool hint_from, bool hint_to) {
    const QColor c = hint_from ? kFromHintColor : (hint_to ? kToHintColor : ink);
    btn->setIcon(renderChevronIcon(left, c, kNavChevronPx));
  };

  apply_hint(left_prev_, /*left=*/true, from_before_l, to_before_l);
  apply_hint(left_next_, /*left=*/false, from_in_gap, to_in_gap);
  apply_hint(right_prev_, /*left=*/true, from_in_gap, to_in_gap);
  apply_hint(right_next_, /*left=*/false, from_after_r, to_after_r);
}

void DualCalendarWidget::onDateClicked(const QDate& date) {
  if (!selecting_) {
    range_from_ = date;
    range_to_ = QDate();
    selecting_ = true;
  } else {
    range_to_ = date;
    if (range_from_ > range_to_) {
      std::swap(range_from_, range_to_);
    }
    hover_date_ = QDate();
    selecting_ = false;
    emit rangeCommitted(range_from_, range_to_);
  }
  broadcastState();
}

void DualCalendarWidget::onDateHovered(const QDate& date) {
  hover_date_ = date;
  broadcastState();
}

void DualCalendarWidget::onHoverLeft() {
  if (hover_date_.isValid()) {
    hover_date_ = QDate();
    broadcastState();
  }
}

void DualCalendarWidget::broadcastState() {
  QDate eff_from = range_from_;
  QDate eff_to = range_to_;
  if (selecting_ && hover_date_.isValid()) {
    eff_to = hover_date_;
  }
  if (eff_from.isValid() && eff_to.isValid() && eff_from > eff_to) {
    std::swap(eff_from, eff_to);
  }
  for (auto* cal : calendars_) {
    cal->setRange(eff_from, eff_to);
    cal->setHoverDate(hover_date_);
  }
  updateNavButtons();
  if (selecting_ && eff_from.isValid() && eff_to.isValid()) {
    emit rangePreview(eff_from, eff_to);
  }
}

// ===========================================================================
// DateRangePicker
// ===========================================================================

DateRangePicker::DateRangePicker(QWidget* parent) : QWidget(parent) {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  // Match the host column's 2px row spacing: without this the preset-row -> date-row
  // gap uses Qt's larger default, so the spacing between the preset buttons and the
  // from/to fields reads bigger than the gaps to the header above / table below.
  main_layout->setSpacing(2);

  auto* preset_row = new QHBoxLayout;
  preset_group_ = new QButtonGroup(this);
  preset_group_->setExclusive(true);

  auto add_preset = [&](int id, const QString& text) -> QPushButton* {
    auto* btn = new QPushButton(text);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    preset_group_->addButton(btn, id);
    preset_row->addWidget(btn);
    return btn;
  };

  all_button_ = add_preset(kPresetAll, "All");
  add_preset(kPresetPast24h, "Past 24h");
  add_preset(kPresetLast7Days, "Last 7 Days");
  add_preset(kPresetLastMonth, "Last Month");
  all_button_->setChecked(true);
  connect(preset_group_, &QButtonGroup::idClicked, this, &DateRangePicker::onPresetClicked);
  main_layout->addLayout(preset_row);

  auto* date_row = new QHBoxLayout;
  from_edit_ = new QLineEdit;
  from_edit_->setPlaceholderText("DD/MM/YYYY");
  // Material "Arrow Right Alt" between from/to, themed to the active ink (re-inked
  // on a live theme switch in changeEvent). A QLabel pixmap, not a text glyph.
  arrow_label_ = new QLabel;
  arrow_label_->setPixmap(renderThemedIcon(QStringLiteral(":/resources/svg/arrow_right_alt.svg"), 18).pixmap(18, 18));
  to_edit_ = new QLineEdit;
  to_edit_->setPlaceholderText(QDate::currentDate().toString("dd/MM/yyyy"));
  calendar_button_ = new QPushButton;
  calendar_button_->setCursor(Qt::PointingHandCursor);
  // 28x28 button with a 24px icon to match the panel's other icon buttons
  // (refresh / search / regex toggles).
  calendar_button_->setFixedSize(28, 28);
  // Themed "Calendar Month" icon (recolored to the active theme's ink); the
  // icon stays static — toggleCalendar() no longer swaps a glyph.
  calendar_button_->setIcon(renderThemedIcon(QStringLiteral(":/resources/svg/calendar_month.svg"), 24));
  calendar_button_->setIconSize(QSize(24, 24));
  date_row->addWidget(from_edit_, 1);
  date_row->addWidget(arrow_label_);
  date_row->addWidget(to_edit_, 1);
  date_row->addWidget(calendar_button_);
  connect(from_edit_, &QLineEdit::textChanged, this, &DateRangePicker::checkCustomState);
  connect(to_edit_, &QLineEdit::textChanged, this, &DateRangePicker::checkCustomState);
  connect(calendar_button_, &QPushButton::clicked, this, &DateRangePicker::toggleCalendar);
  main_layout->addLayout(date_row);
}

DateRangePicker::~DateRangePicker() {
  delete overlay_;
}

void DateRangePicker::setEarliestDate(const QDate& date) {
  from_edit_->setPlaceholderText(date.isValid() ? date.toString("dd/MM/yyyy") : QStringLiteral("DD/MM/YYYY"));
}

void DateRangePicker::setLatestDate(const QDate& date) {
  to_edit_->setPlaceholderText(date.isValid() ? date.toString("dd/MM/yyyy") : QStringLiteral("DD/MM/YYYY"));
}

void DateRangePicker::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  // Default the "to" placeholder to today only when no latest hint was pushed.
  if (to_edit_->placeholderText().isEmpty()) {
    to_edit_->setPlaceholderText(QDate::currentDate().toString("dd/MM/yyyy"));
  }

  if (!overlay_ && window()) {
    overlay_ = new QWidget(window());
    overlay_->setObjectName("PickerOverlay");
    updateOverlayStyle();
    auto* overlay_layout = new QVBoxLayout(overlay_);
    dual_calendar_ = new DualCalendarWidget;
    overlay_layout->addWidget(dual_calendar_);
    time_picker_ = new TimePickerWidget;
    overlay_layout->addWidget(time_picker_);
    overlay_->setVisible(false);
    connect(dual_calendar_, &DualCalendarWidget::rangeCommitted, this, &DateRangePicker::onCalendarRangeCommitted);
    connect(dual_calendar_, &DualCalendarWidget::rangePreview, this, &DateRangePicker::onCalendarRangePreview);
    connect(time_picker_, &TimePickerWidget::timeChanged, this, &DateRangePicker::onTimeChanged);
    window()->installEventFilter(this);
  }
}

void DateRangePicker::changeEvent(QEvent* event) {
  // Re-theme on a live theme toggle. Both StyleChange and PaletteChange are
  // handled: the app pins QPalette but a theme swap can still arrive as either,
  // so cover both. The calendars/overlay paint from pickerTokens() (keyed off
  // the persisted theme), so we re-apply the overlay stylesheet, re-render the
  // button icon in the new ink, and force a repaint of the overlay subtree
  // (it lives under window(), not under this widget, so it won't repaint on its
  // own from a change event delivered here).
  if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
    updateOverlayStyle();
    if (calendar_button_) {
      calendar_button_->setIcon(renderThemedIcon(QStringLiteral(":/resources/svg/calendar_month.svg"), 24));
    }
    if (arrow_label_) {
      arrow_label_->setPixmap(
          renderThemedIcon(QStringLiteral(":/resources/svg/arrow_right_alt.svg"), 18).pixmap(18, 18));
    }
    if (dual_calendar_) {
      dual_calendar_->retheme();  // re-ink the nav chevrons to the new theme
    }
    if (overlay_) {
      const auto children = overlay_->findChildren<QWidget*>();
      overlay_->update();
      for (auto* child : children) {
        child->update();
      }
    }
  }
  QWidget::changeEvent(event);
}

void DateRangePicker::updateOverlayStyle() {
  if (!overlay_) {
    return;
  }
  // Drive the overlay popup from the theme tokens so it matches the active QSS
  // theme. Same precedent as the app's QFrame#DiagnosticsPopup (background =
  // dark_background, 1px solid border_default), with square corners. palette()
  // can't be used here: the app pins QPalette at Fusion defaults regardless of
  // theme, so it would resolve to Fusion light-grey on both.
  const PickerTokens tok = pickerTokens();
  overlay_->setStyleSheet(QStringLiteral("QWidget#PickerOverlay { background-color: %1; border: 1px solid %2; }")
                              .arg(tok.surface.name(), tok.border.name()));
}

void DateRangePicker::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (overlay_ && overlay_->isVisible()) {
    repositionOverlay();
  }
}

bool DateRangePicker::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window() && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
    if (overlay_ && overlay_->isVisible()) {
      repositionOverlay();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void DateRangePicker::repositionOverlay() {
  if (!overlay_ || !calendar_button_) {
    return;
  }
  QPoint anchor = calendar_button_->mapTo(window(), QPoint(calendar_button_->width(), calendar_button_->height()));
  int x = anchor.x() + 4;
  int y = anchor.y() + 2;
  int overlay_width = overlay_->sizeHint().width();
  int max_x = window()->width() - overlay_width - 4;
  if (x > max_x) {
    x = max_x;
  }
  if (x < 0) {
    x = 0;
  }
  overlay_->move(x, y);
  overlay_->resize(overlay_->sizeHint());
  overlay_->raise();
}

void DateRangePicker::onPresetClicked(int id) {
  all_button_->setText("All");
  applyPreset(id);
  syncCalendarToFields();
  emitFilter();
}

void DateRangePicker::syncCalendarToFields() {
  if (!dual_calendar_) {
    return;
  }
  const QString fmt = QStringLiteral("dd/MM/yyyy");
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  dual_calendar_->setExternalRange(from, to);
}

void DateRangePicker::applyPreset(int id) {
  QDate today = QDate::currentDate();
  QDate from, to;
  switch (id) {
    case kPresetAll:
      break;
    case kPresetPast24h:
      from = today.addDays(-1);
      to = today;
      break;
    case kPresetLast7Days:
      from = today.addDays(-6);
      to = today;
      break;
    case kPresetLastMonth: {
      QDate first_of_month(today.year(), today.month(), 1);
      to = first_of_month.addDays(-1);
      from = QDate(to.year(), to.month(), 1);
      break;
    }
  }
  updateFieldsFromPreset(from, to);
}

void DateRangePicker::updateFieldsFromPreset(const QDate& from, const QDate& to) {
  const QString fmt = QStringLiteral("dd/MM/yyyy");
  QSignalBlocker fb(from_edit_);
  QSignalBlocker tb(to_edit_);
  from_edit_->setText(from.isValid() ? from.toString(fmt) : QString());
  to_edit_->setText(to.isValid() ? to.toString(fmt) : QString());
}

int DateRangePicker::matchingPreset() const {
  const QString fmt = QStringLiteral("dd/MM/yyyy");
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  QDate today = QDate::currentDate();

  if (from_edit_->text().isEmpty() && to_edit_->text().isEmpty()) {
    return kPresetAll;
  }
  if (!from.isValid() || !to.isValid()) {
    return -1;
  }
  if (from == today.addDays(-1) && to == today) {
    return kPresetPast24h;
  }
  if (from == today.addDays(-6) && to == today) {
    return kPresetLast7Days;
  }
  QDate first_of_month(today.year(), today.month(), 1);
  QDate last_of_prev = first_of_month.addDays(-1);
  QDate first_of_prev(last_of_prev.year(), last_of_prev.month(), 1);
  if (from == first_of_prev && to == last_of_prev) {
    return kPresetLastMonth;
  }
  return -1;
}

void DateRangePicker::checkCustomState() {
  int preset = matchingPreset();
  if (preset >= 0) {
    all_button_->setText("All");
    preset_group_->button(preset)->setChecked(true);
  } else {
    all_button_->setText("Custom");
    all_button_->setChecked(true);
  }
  syncCalendarToFields();
  emitFilter();
}

void DateRangePicker::toggleCalendar() {
  if (!overlay_) {
    return;
  }
  calendar_visible_ = !calendar_visible_;
  overlay_->setVisible(calendar_visible_);
  // Icon stays static (no glyph swap) — the open/closed state is conveyed by
  // the overlay's visibility.
  if (calendar_visible_) {
    repositionOverlay();
  }
}

void DateRangePicker::onCalendarRangeCommitted(const QDate& from, const QDate& to) {
  updateFieldsFromPreset(from, to);
  checkCustomState();
  if (time_picker_) {
    time_picker_->setFromDate(from);
    time_picker_->setToDate(to);
  }
}

void DateRangePicker::onCalendarRangePreview(const QDate& from, const QDate& to) {
  updateFieldsFromPreset(from, to);
  emitFilter();
}

void DateRangePicker::onTimeChanged() {
  emitFilter();
}

RangeFilter DateRangePicker::buildFilter() const {
  RangeFilter f;
  const QString fmt = QStringLiteral("dd/MM/yyyy");
  QDate from = QDate::fromString(from_edit_->text(), fmt);
  QDate to = QDate::fromString(to_edit_->text(), fmt);
  if (from.isValid()) {
    f.date_from = from;
  }
  if (to.isValid()) {
    f.date_to = to;
  }
  if (time_picker_) {
    f.from_time = time_picker_->fromTime();
    QTime to_t = time_picker_->toTime();
    f.to_time = QTime(to_t.hour(), to_t.minute(), 59, 999);
  }
  return f;
}

void DateRangePicker::emitFilter() {
  emit filterChanged(buildFilter());
}

}  // namespace PJ
