#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QString>
#include <QWidget>
#include <utility>

namespace PJ {

// QLabel variant that re-elides its text whenever its width changes and
// can also be driven from the outside. Below a configurable minimum the
// label hides itself so its space is reclaimed by sibling layout items;
// when its parent re-expands, applyAvailableWidth() (called from
// whoever drives the row) brings it back.
//
// The dual interface (own resizeEvent + applyAvailableWidth) is needed
// because hidden widgets don't receive resize events: once we go to
// 0 px we have to be told from the outside to come back.
class ElidingLabel : public QLabel {
 public:
  explicit ElidingLabel(QWidget* parent = nullptr) : QLabel(parent) {
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

  void setFullText(QString text) {
    full_text_ = std::move(text);
    // Re-elide using current width without waiting for a resize tick.
    applyAvailableWidth(width());
  }

  [[nodiscard]] QString fullText() const {
    return full_text_;
  }

  void setHideBelowWidth(int px) {
    hide_below_ = px;
    applyAvailableWidth(width());
  }

  void setElideMode(Qt::TextElideMode mode) {
    elide_mode_ = mode;
    applyAvailableWidth(width());
  }

  // Externally-driven update: parent passes the budget it has carved
  // out for the label. This is the path that can RE-SHOW a previously
  // hidden label (which wouldn't otherwise see resize events).
  void applyAvailableWidth(int width_px) {
    if (width_px < hide_below_) {
      if (isVisible()) {
        setVisible(false);
      }
      return;
    }
    if (!isVisible()) {
      setVisible(true);
    }
    const QFontMetrics metrics(font());
    setText(metrics.elidedText(full_text_, elide_mode_, width_px));
  }

 protected:
  // Layout-driven update: when sibling widgets resize naturally the
  // label receives a resize event and re-elides without needing the
  // parent to call applyAvailableWidth.
  void resizeEvent(QResizeEvent* event) override {
    QLabel::resizeEvent(event);
    applyAvailableWidth(width());
  }

 private:
  QString full_text_;
  Qt::TextElideMode elide_mode_ = Qt::ElideRight;
  int hide_below_ = 16;
};

}  // namespace PJ
