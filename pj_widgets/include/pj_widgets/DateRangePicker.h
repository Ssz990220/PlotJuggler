#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Date/time range picker bundle. Registered with the host's PjUiLoader so
// plugin .ui files can declare a "DateRangePicker" by class name. Comprises:
//   CalendarWidget        — single painted month grid
//   TimePickerWidget      — from/to 12-hour time
//   DualCalendarWidget    — two mediated calendars + nav
//   DateRangePicker       — inline presets + from/to fields + calendar overlay
// DateRangePicker emits filterChanged(RangeFilter); the host binding serializes
// it into a dateRangeChanged event.

#include <QDate>
#include <QList>
#include <QTime>
#include <QWidget>
#include <optional>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace PJ {

// --- CalendarWidget -------------------------------------------------------

class CalendarWidget : public QWidget {
  Q_OBJECT
 public:
  explicit CalendarWidget(QWidget* parent = nullptr);

  void setMonth(int year, int month);
  int year() const {
    return year_;
  }
  int month() const {
    return month_;
  }

  void setMediated(bool mediated);
  bool isMediated() const {
    return mediated_;
  }

  void setRange(const QDate& from, const QDate& to);
  void setHoverDate(const QDate& date);
  void clearRange();

  QDate rangeFrom() const {
    return range_from_;
  }
  QDate rangeTo() const {
    return range_to_;
  }

 signals:
  void dateClicked(const QDate& date);
  void dateDoubleClicked(const QDate& date);
  void dateHovered(const QDate& date);
  void hoverLeft();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private:
  QDate dateAtPosition(const QPoint& pos) const;
  QRect cellRect(int row, int col) const;
  int headerHeight() const;
  int firstDayColumn() const;

  int year_;
  int month_;
  QDate range_from_;
  QDate range_to_;
  QDate hover_date_;

  enum class State { Idle, Selecting, Committed };
  State state_ = State::Idle;
  bool mediated_ = false;
};

// --- TimePickerWidget -----------------------------------------------------

class TimePickerWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimePickerWidget(QWidget* parent = nullptr);

  void setFromDate(const QDate& date);
  void setToDate(const QDate& date);

  QTime fromTime() const;
  QTime toTime() const;

 signals:
  void timeChanged();

 private:
  QTime timeFrom12Hour(int hour12, int minute, const QString& ampm) const;

  QLabel* from_date_label_;
  QSpinBox* from_hour_;
  QSpinBox* from_minute_;
  QComboBox* from_am_pm_;
  QLabel* to_date_label_;
  QSpinBox* to_hour_;
  QSpinBox* to_minute_;
  QComboBox* to_am_pm_;
};

// --- DualCalendarWidget ---------------------------------------------------

class DualCalendarWidget : public QWidget {
  Q_OBJECT
 public:
  explicit DualCalendarWidget(QWidget* parent = nullptr);

  void setExternalRange(const QDate& from, const QDate& to);

 signals:
  void rangeCommitted(const QDate& from, const QDate& to);
  void rangePreview(const QDate& from, const QDate& to);

 private slots:
  void onDateClicked(const QDate& date);
  void onDateHovered(const QDate& date);
  void onHoverLeft();
  void leftPrev();
  void leftNext();
  void rightPrev();
  void rightNext();

 private:
  void updateCalendars();
  void updateNavButtons();
  void broadcastState();
  static void advanceMonth(int& year, int& month, int delta);

  QList<CalendarWidget*> calendars_;
  QPushButton* left_prev_;
  QPushButton* left_next_;
  QPushButton* right_prev_;
  QPushButton* right_next_;

  int left_year_;
  int left_month_;
  int left_max_year_;
  int left_max_month_;
  int right_year_;
  int right_month_;
  int right_min_year_;
  int right_min_month_;

  bool selecting_ = false;
  QDate range_from_;
  QDate range_to_;
  QDate hover_date_;
};

// --- RangeFilter + DateRangePicker -----------------------------------------

struct RangeFilter {
  std::optional<QDate> date_from;
  std::optional<QDate> date_to;
  QTime from_time = QTime(0, 0);
  QTime to_time = QTime(23, 59, 59, 999);
};

class DateRangePicker : public QWidget {
  Q_OBJECT
 public:
  explicit DateRangePicker(QWidget* parent = nullptr);
  ~DateRangePicker() override;

  void setEarliestDate(const QDate& date);
  void setLatestDate(const QDate& date);

 signals:
  void filterChanged(const RangeFilter& filter);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void changeEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

 private slots:
  void onPresetClicked(int id);
  void checkCustomState();
  void toggleCalendar();
  void onCalendarRangeCommitted(const QDate& from, const QDate& to);
  void onCalendarRangePreview(const QDate& from, const QDate& to);
  void onTimeChanged();

 private:
  void emitFilter();
  void repositionOverlay();
  void updateOverlayStyle();
  RangeFilter buildFilter() const;
  void applyPreset(int id);
  void updateFieldsFromPreset(const QDate& from, const QDate& to);
  int matchingPreset() const;
  void syncCalendarToFields();

  static constexpr int kPresetAll = 0;
  static constexpr int kPresetPast24h = 1;
  static constexpr int kPresetLast7Days = 2;
  static constexpr int kPresetLastMonth = 3;

  QButtonGroup* preset_group_ = nullptr;
  QPushButton* all_button_ = nullptr;
  QLineEdit* from_edit_ = nullptr;
  QLineEdit* to_edit_ = nullptr;
  QPushButton* calendar_button_ = nullptr;

  QWidget* overlay_ = nullptr;
  DualCalendarWidget* dual_calendar_ = nullptr;
  TimePickerWidget* time_picker_ = nullptr;

  bool calendar_visible_ = false;
};

}  // namespace PJ
