// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Standalone visual demo of the reusable Timeline control. Fabricated tracks,
// no runtime: drag a bar to shift its display offset, press Align to snap all
// starts to the earliest, wheel to zoom. Run: pj_widgets_timeline_demo

#include <QApplication>

#include "pj_widgets/Timeline.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  PJ::Timeline widget;
  widget.setTracks({
      {.id = 1,
       .name = "bag_a",
       .t_min_ns = 0,
       .t_max_ns = 30'000'000'000,
       .offset_ns = 0,
       .color = QColor(0x4F, 0xC3, 0xF7)},
      {.id = 2,
       .name = "bag_b",
       .t_min_ns = 5'000'000'000,
       .t_max_ns = 40'000'000'000,
       .offset_ns = 0,
       .color = QColor(0x81, 0xC7, 0x84)},
  });
  widget.resize(900, 200);
  widget.show();
  return app.exec();
}
