#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QByteArray>
#include <QDebug>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QString>
#include <QSvgRenderer>
#include <map>

namespace PJ {

constexpr const char* kThemeSettingsKey = "StyleSheet::theme";

inline QString currentTheme() {
  return QSettings().value(kThemeSettingsKey, "light").toString();
}

// Recolour every visible stroke/fill in an SVG document to the active
// theme's ink. Handles three common shapes:
//   1. Icons baked with the PJ4 palette (`#3D3D3D` for light theme,
//      `#E0E0E0` for dark) by the download/derive pipeline — the two
//      palette colors are swapped so the icon renders in the active
//      theme regardless of which one it was authored for.
//   2. Legacy black/white SVGs (the app logo, hand-edited snippets) —
//      `#000000` / `#ffffff` are folded onto the palette so they flip
//      with the theme too.
//   3. Material Symbols SVGs without any explicit fill — we inject a
//      `fill="..."` on the root `<svg>` so paths inherit the theme ink.
inline void RecolorSvgInk(QByteArray& svg_data, bool light_theme) {
  // PJ4 theme palette: light = Jet, dark = Platinum.
  const QByteArray ink = light_theme ? QByteArray("#3D3D3D") : QByteArray("#E0E0E0");
  const QByteArray opposite = light_theme ? QByteArray("#E0E0E0") : QByteArray("#3D3D3D");

  // (1) Palette swap.
  svg_data.replace(opposite, ink);

  // (2) Legacy black/white — fold onto the palette so monochrome assets
  // (PJ3 icons, the logo) flip with the active theme.
  svg_data.replace("#000000", ink);
  svg_data.replace("#ffffff", opposite);

  // (3) Root-tag fill injection: if the SVG has no `fill` on its root
  // element, give it one so any per-path-fill-less children inherit it.
  const int svg_open = static_cast<int>(svg_data.indexOf("<svg"));
  if (svg_open < 0) {
    return;
  }
  const int tag_end = static_cast<int>(svg_data.indexOf('>', svg_open));
  if (tag_end <= svg_open) {
    return;
  }
  const QByteArray open_tag = svg_data.mid(svg_open, tag_end - svg_open);
  if (open_tag.contains("fill=\"")) {
    return;
  }
  const QByteArray fill_attr = " fill=\"" + ink + "\"";
  svg_data.insert(tag_end, fill_attr);
}

// Load an SVG from a resource path, recoloring monochrome content (#000000 /
// #ffffff) for the requested theme. Results are cached per (path, theme).
// Caller must use this on the GUI thread only — the cache maps are not locked.
inline const QPixmap& LoadSvg(const QString& filename, const QString& style_name = "light") {
  static std::map<QString, QPixmap> light_images;
  static std::map<QString, QPixmap> dark_images;
  const bool light_theme = style_name.contains("light");

  auto* stored_images = light_theme ? &light_images : &dark_images;

  auto it = stored_images->find(filename);
  if (it != stored_images->end()) {
    return it->second;
  }

  QFile file(filename);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    qWarning("PJ::LoadSvg: cannot open %s", qPrintable(filename));
    static const QPixmap fallback;
    return fallback;
  }
  QByteArray svg_data = file.readAll();
  file.close();

  RecolorSvgInk(svg_data, light_theme);

  QSvgRenderer renderer(svg_data);
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();

  it = stored_images->insert({filename, QPixmap::fromImage(image)}).first;
  return it->second;
}

// Rasterise a monochrome SVG at the requested logical size, honouring the
// caller's devicePixelRatio so QLabel::setPixmap stays crisp on HiDPI
// screens. Use this instead of `LoadSvg(...).scaled(w, h, ...)` when the
// rendered size differs from LoadSvg's 64x64 cache: scaling the cached
// pixmap blurs; this renders fresh at logical_size * dpr pixels and
// stamps the DPR on the result so Qt halves it back to logical units.
// Not cached — call sparingly (e.g. once per widget construction).
inline QPixmap RenderSvgPixmap(
    const QString& filename, const QString& style_name, const QSize& logical_size, qreal dpr) {
  QFile file(filename);
  if (!file.open(QFile::ReadOnly | QFile::Text)) {
    qWarning("PJ::RenderSvgPixmap: cannot open %s", qPrintable(filename));
    return {};
  }
  QByteArray svg_data = file.readAll();
  file.close();
  const bool light_theme = style_name.contains("light");
  RecolorSvgInk(svg_data, light_theme);
  QSvgRenderer renderer(svg_data);
  const QSize physical = logical_size * dpr;
  QImage image(physical, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();
  QPixmap pm = QPixmap::fromImage(image);
  pm.setDevicePixelRatio(dpr);
  return pm;
}

}  // namespace PJ
