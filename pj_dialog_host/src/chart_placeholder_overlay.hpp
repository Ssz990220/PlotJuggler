#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QEvent>
#include <QLabel>
#include <QString>
#include <QWidget>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

// Floating, centered translucent hint banner shown over an empty chart frame
// (e.g. a drag-and-drop prompt). Mirrors the Timeline's lock overlay: transparent
// to the mouse, sized to its text, and re-centered whenever the host frame
// resizes. The host shows it while the chart has no series and hides it once data
// arrives.
class ChartPlaceholderOverlay : public QLabel {
  Q_OBJECT
 public:
  explicit ChartPlaceholderOverlay(QWidget* host) : QLabel(host) {
    const auto fw_theme = theme::appTheme();
    setObjectName(QStringLiteral("chartPlaceholderOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAlignment(Qt::AlignCenter);
    setWordWrap(true);
    setStyleSheet(
        QStringLiteral(
            "QLabel#chartPlaceholderOverlay { background-color: %1; color: %2; "
            "border: 1px solid %3; border-radius: %4px; padding: %5px %6px; "
            "font-size: 13px; font-weight: 600; }")
            .arg(
                theme::overlay(theme::Overlay::Hud, fw_theme).name(QColor::HexArgb),
                theme::onOverlayHud(fw_theme).name(QColor::HexArgb),
                theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, fw_theme).name(QColor::HexArgb))
            .arg(theme::radius(theme::Radius::Dialog))
            .arg(theme::space(theme::Space::Comfortable, fw_theme))
            .arg(theme::space(theme::Space::Section, fw_theme)));
    if (host != nullptr) {
      host->installEventFilter(this);
    }
  }

  // Center over the host frame, sized to the text (capped to the frame width).
  void recenter() {
    QWidget* host = parentWidget();
    if (host == nullptr) {
      return;
    }
    // Only recompute when the host actually changed size. This lets recenter() be
    // called from the Paint eventFilter every repaint without churning the layout
    // (the wrap toggle + resize would otherwise re-fire on each paint), while still
    // re-centering the first time the host reaches a new (e.g. final) size.
    if (host->size() == last_host_size_) {
      return;
    }
    last_host_size_ = host->size();
    // Cap the banner to the host width (small inset); wrap only when the
    // single-line text would overflow it.
    const int avail_w = std::max(80, host->width() - 48);
    setWordWrap(false);
    int w = sizeHint().width();
    if (w > avail_w) {
      setWordWrap(true);
      w = avail_w;
    }
    // heightForWidth() is the reliable wrapped height. QLabel::sizeHint() does not
    // track a constrained width, so adjustSize() mis-sizes (and clips) a wrapped
    // banner — which then centers off, since the centering maths use that wrong
    // height. When not wrapping, sizeHint() is exact. resize() (not setFixed*)
    // keeps it re-measurable on every recenter.
    const int h = wordWrap() ? heightForWidth(w) : sizeHint().height();
    resize(w, std::max(h, 1));
    move(std::max(0, (host->width() - width()) / 2), std::max(0, (host->height() - height()) / 2));
    raise();
  }

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override {
    // Resize/Show cover the common cases; Paint is the timing-proof safety net —
    // it fires after the final layout settles, so even when the host frame reaches
    // its final size through a path that delivers no Resize event the overlay saw
    // (e.g. the frame was sized before this overlay existed, during dialog
    // construction), the first paint re-centers it. recenter() is a no-op once the
    // geometry already matches, so this does not churn on ordinary repaints.
    if (obj == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Paint)) {
      recenter();
    }
    return QLabel::eventFilter(obj, event);
  }

 private:
  QSize last_host_size_;  // guards recenter() against re-running at an unchanged host size
};

}  // namespace PJ
