// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/IntScrubber.h"

#include <algorithm>

namespace PJ {

IntScrubber::IntScrubber(QWidget* parent) : ScrubberBase(parent) {}

int IntScrubber::clamp(int v) const {
  return std::clamp(v, minimum_, maximum_);
}

void IntScrubber::setValue(int v) {
  const int clamped = clamp(v);
  if (clamped == value_) {
    return;
  }
  value_ = clamped;
  emit valueChanged(value_);
  valueRepaint();
}

void IntScrubber::setRange(int min, int max) {
  minimum_ = std::min(min, max);
  maximum_ = std::max(min, max);
  setValue(value_);
  valueRepaint();
}

void IntScrubber::setMinimum(int v) {
  minimum_ = v;
  if (maximum_ < minimum_) {
    maximum_ = minimum_;
  }
  setValue(value_);
}

void IntScrubber::setMaximum(int v) {
  maximum_ = v;
  if (minimum_ > maximum_) {
    minimum_ = maximum_;
  }
  setValue(value_);
}

void IntScrubber::setSingleStep(int v) {
  single_step_ = std::max(1, v);
}

void IntScrubber::setSuffix(const QString& s) {
  suffix_ = s;
  valueRepaint();
}

void IntScrubber::setPrefix(const QString& s) {
  prefix_ = s;
  valueRepaint();
}

void IntScrubber::stepBy(int steps_signed) {
  setValue(value_ + steps_signed * single_step_);
}

QString IntScrubber::displayText() const {
  return prefix_ + QString::number(value_) + suffix_;
}

bool IntScrubber::commitText(const QString& text) {
  QString stripped = text;
  if (!prefix_.isEmpty() && stripped.startsWith(prefix_)) {
    stripped = stripped.mid(prefix_.size());
  }
  if (!suffix_.isEmpty() && stripped.endsWith(suffix_)) {
    stripped.chop(suffix_.size());
  }
  bool ok = false;
  const int parsed = stripped.trimmed().toInt(&ok);
  if (!ok) {
    return false;
  }
  setValue(parsed);
  return true;
}

}  // namespace PJ
