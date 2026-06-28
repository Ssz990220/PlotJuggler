// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "Splashscreen.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QRectF>
#include <QSettings>
#include <QStringList>
#include <QSvgRenderer>
#include <Qt>
#include <algorithm>

#include "pj_widgets/ThemeColors.h"

namespace PJ {
namespace {

// Picks a random "funny meme" from the bundled pool (:/resources/memes),
// avoiding the handful most recently shown (persisted in QSettings) so
// consecutive launches don't repeat until the pool cycles. The pool is
// enumerated at runtime, so adding a meme is a pure resource change — no code
// edit. Returns a null QPixmap when the pool is empty.
QPixmap getFunnySplashscreen() {
  QDir memes_dir(QStringLiteral(":/resources/memes"));
  const QStringList files = memes_dir.entryList({QStringLiteral("*.jpg")}, QDir::Files, QDir::Name);
  if (files.isEmpty()) {
    return {};
  }

  QSettings settings;
  QStringList recent = settings.value(QStringLiteral("previousFunnyMemesList")).toStringList();
  // Never block more entries than (pool - 1), otherwise the pick loop below
  // could never find an unseen meme and would spin forever.
  const qsizetype avoid_count = std::min<qsizetype>(recent.size(), files.size() - 1);
  const QStringList avoid = recent.mid(recent.size() - avoid_count);

  QString chosen;
  do {
    chosen = files.at(QRandomGenerator::global()->bounded(static_cast<int>(files.size())));
  } while (avoid.contains(chosen));

  // Keep the 10 most-recent picks (PJ3's history depth).
  recent.append(chosen);
  while (recent.size() > 10) {
    recent.removeFirst();
  }
  settings.setValue(QStringLiteral("previousFunnyMemesList"), recent);

  // Cap oversized memes so a large/high-res image doesn't dominate the screen;
  // smaller ones are left untouched (never upscaled).
  constexpr int kMaxMemeWidth = 600;
  const QPixmap pixmap(memes_dir.filePath(chosen));
  return pixmap.width() > kMaxMemeWidth ? pixmap.scaledToWidth(kMaxMemeWidth, Qt::SmoothTransformation) : pixmap;
}

// The pool of subtitles for the "serious" splashscreen; one is picked at random
// each launch.
const QStringList& seriousSubtitles() {
  static const QStringList kSubtitles = {
      QStringLiteral("Now you're plotting"),
      QStringLiteral("The plot thickens"),
      QStringLiteral("Hold my rosbag"),
      QStringLiteral("Make your data confess"),
      QStringLiteral("More than just curves now"),
      QStringLiteral("Better than it has any right to be"),
      QStringLiteral("Objectively the best. Probably."),
      QStringLiteral("Still better than printf"),
      QStringLiteral("Friends don't let friends use printf"),
      QStringLiteral("Lovingly over-engineered"),
      QStringLiteral("Suspiciously good for a free tool"),
      QStringLiteral("Opening PlotJuggler. Again."),
      QStringLiteral("Curves were just the beginning"),
      QStringLiteral("Mostly written by an AI"),
      QStringLiteral("An AI helped. Allegedly."),
  };
  return kSubtitles;
}

// Renders the "serious" splashscreen: the PlotJuggler logo + wordmark (P blue,
// J magenta, the rest slate grey) over a soft light gradient, with a random
// subtitle underneath. Composed with QPainter rather than baked into an image
// so every element stays crisp at any DPI and the subtitle can vary per launch.
QPixmap makeSeriousSplashscreen() {
  constexpr int kW = 720;
  constexpr int kH = 300;
  const qreal dpr = qMax(1.0, qApp->devicePixelRatio());

  QPixmap pixmap(QSize(kW, kH) * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);

  QPainter p(&pixmap);
  p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);

  const QRectF canvas(0, 0, kW, kH);

  // Soft, cool, near-white background gradient — modern and unobtrusive.
  QLinearGradient bg(canvas.topLeft(), canvas.bottomRight());
  bg.setColorAt(0.0, QColor(0xff, 0xff, 0xff));
  bg.setColorAt(1.0, QColor(0xea, 0xf0, 0xfb));
  p.fillRect(canvas, bg);

  // A thin brand-gradient accent along the bottom edge.
  QLinearGradient accent(canvas.bottomLeft(), canvas.bottomRight());
  accent.setColorAt(0.0, theme::kBlue);
  accent.setColorAt(1.0, theme::kPurple);
  p.fillRect(QRectF(0, kH - 4, kW, 4), accent);

  // Logo, vertically centred on the left.
  constexpr qreal kLogoSize = 156;
  const QRectF logo_rect(56, (kH - kLogoSize) / 2.0, kLogoSize, kLogoSize);
  QSvgRenderer logo(QStringLiteral(":/resources/plotjuggler.svg"));
  if (logo.isValid()) {
    logo.render(&p, logo_rect);
  }

  const qreal text_x = logo_rect.right() + 44;
  const qreal wordmark_baseline = kH / 2.0 - 18;

  // Wordmark "PlotJuggler": draw the whole word in slate grey, then overdraw the
  // two juggling capitals in the brand colours (P blue, J magenta). The coloured
  // glyphs land exactly on top of the grey ones, so the spacing is identical to
  // a single drawText of the full word.
  QFont wordmark_font;
  wordmark_font.setFamilies({QStringLiteral("Khula"), QStringLiteral("Roboto"), wordmark_font.defaultFamily()});
  wordmark_font.setPixelSize(76);
  wordmark_font.setWeight(QFont::Thin);  // thinnest available weight, for an airy banner look
  p.setFont(wordmark_font);
  const QString wordmark = QStringLiteral("PlotJuggler");
  const QFontMetricsF wordmark_fm(wordmark_font);
  const qreal wordmark_width = wordmark_fm.horizontalAdvance(wordmark);
  const QColor slate(0x63, 0x6a, 0x78);
  p.setPen(slate);
  p.drawText(QPointF(text_x, wordmark_baseline), wordmark);
  p.setPen(theme::kBlue);
  p.drawText(QPointF(text_x, wordmark_baseline), QStringLiteral("P"));
  p.setPen(theme::kPurple);
  p.drawText(
      QPointF(text_x + wordmark_fm.horizontalAdvance(QStringLiteral("Plot")), wordmark_baseline), QStringLiteral("J"));

  // Random subtitle, centred horizontally under the wordmark, in the same slate.
  QFont subtitle_font;
  subtitle_font.setFamilies({QStringLiteral("Roboto"), subtitle_font.defaultFamily()});
  subtitle_font.setPixelSize(26);
  subtitle_font.setWeight(QFont::Light);  // thin, airy weight to match the banner
  p.setFont(subtitle_font);
  p.setPen(slate);
  const QStringList& subs = seriousSubtitles();
  const QString subtitle = subs.at(QRandomGenerator::global()->bounded(static_cast<int>(subs.size())));
  const qreal subtitle_width = QFontMetricsF(subtitle_font).horizontalAdvance(subtitle);
  p.drawText(QPointF(text_x + (wordmark_width - subtitle_width) / 2.0, wordmark_baseline + 56), subtitle);

  // Version, small and unobtrusive in the corner.
  QFont version_font;
  version_font.setPixelSize(13);
  p.setFont(version_font);
  p.setPen(QColor(0xa6, 0xb0, 0xbd));
  p.drawText(
      QRectF(0, kH - 30, kW - 18, 20), Qt::AlignRight | Qt::AlignVCenter, QCoreApplication::applicationVersion());

  p.end();
  return pixmap;
}

}  // namespace

QPixmap makeStartupSplash() {
  QSettings settings;
  const QString mode = settings.value(kSplashModeKey, kSplashModeMemes).toString();
  return mode == kSplashModeSerious ? makeSeriousSplashscreen() : getFunnySplashscreen();
}

}  // namespace PJ
