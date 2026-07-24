// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tiny standalone demo of the dialog building-block widgets added for the
// plugin SDK 0.4.0 dialog contract: RangeSlider (two-handle, with floating
// labels) and DateRangePicker. Run: pj_widgets_demo

#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>

#include <QApplication>
#include <QDate>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
using namespace Qt::StringLiterals;

namespace {

QString dateOrUnbounded(const std::optional<QDate>& d) {
  return d ? d->toString(Qt::ISODate) : u"(unbounded)"_s;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  QWidget window;
  window.setWindowTitle(u"pj_widgets demo — RangeSlider & DateRangePicker"_s);
  auto* root = new QVBoxLayout(&window);

  // --- RangeSlider ---------------------------------------------------------
  auto* slider_box = new QGroupBox(u"RangeSlider (two handles)"_s);
  auto* slider_layout = new QVBoxLayout(slider_box);

  auto* slider = new PJ::RangeSlider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles);
  slider->setMinimum(0);
  slider->setMaximum(1000);
  slider->setLowerValue(200);
  slider->setUpperValue(800);
  slider->setFloatingLabelsVisible(true);
  // Treat the [0,1000] track as a 1000s window so the labels read as durations.
  slider->setLabelFormatter([](double pos) { return u"%1s"_s.arg(static_cast<int>(pos)); });
  slider->setCenterLabelFormatter(
      [](double lo, double hi) { return u"%1s selected"_s.arg(static_cast<int>(hi - lo)); });
  slider_layout->addWidget(slider);

  auto* range_label = new QLabel;
  slider_layout->addWidget(range_label);
  auto update_range = [slider, range_label]() {
    range_label->setText(u"lower = %1   upper = %2"_s.arg(slider->getLowerValue()).arg(slider->getUpperValue()));
  };
  QObject::connect(slider, &PJ::RangeSlider::lowerValueChanged, range_label, [update_range](int) { update_range(); });
  QObject::connect(slider, &PJ::RangeSlider::upperValueChanged, range_label, [update_range](int) { update_range(); });
  update_range();
  root->addWidget(slider_box);

  // --- DateRangePicker -----------------------------------------------------
  auto* picker_box = new QGroupBox(u"DateRangePicker"_s);
  auto* picker_layout = new QVBoxLayout(picker_box);

  auto* picker = new PJ::DateRangePicker;
  picker->setEarliestDate(QDate(2016, 4, 29));
  picker->setLatestDate(QDate::currentDate());
  picker_layout->addWidget(picker);

  auto* date_label = new QLabel(u"(no range selected)"_s);
  picker_layout->addWidget(date_label);
  QObject::connect(picker, &PJ::DateRangePicker::filterChanged, date_label, [date_label](const PJ::RangeFilter& f) {
    date_label->setText(u"from = %1   to = %2"_s.arg(dateOrUnbounded(f.date_from), dateOrUnbounded(f.date_to)));
  });
  root->addWidget(picker_box);

  window.resize(520, 420);
  window.show();
  return app.exec();
}
