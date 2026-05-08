#pragma once

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

  if (light_theme) {
    svg_data.replace("#000000", "#111111");
    svg_data.replace("#ffffff", "#dddddd");
  } else {
    svg_data.replace("#000000", "#dddddd");
    svg_data.replace("#ffffff", "#111111");
  }

  QSvgRenderer renderer(svg_data);
  QImage image(64, 64, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  renderer.render(&painter);
  painter.end();

  it = stored_images->insert({filename, QPixmap::fromImage(image)}).first;
  return it->second;
}

}  // namespace PJ
