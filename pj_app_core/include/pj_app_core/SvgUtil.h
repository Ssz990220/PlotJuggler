#pragma once

#include <QByteArray>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>

#include <map>

namespace PJ {

// Load an SVG from a resource path, recoloring monochrome content (#000000 /
// #ffffff) for the requested theme. Results are cached per (path, theme).
// Ported verbatim from PJ3 plotjuggler_base/include/PlotJuggler/svg_util.h.
inline const QPixmap& LoadSvg(QString filename, QString style_name = "light") {
  static std::map<QString, QPixmap> light_images;
  static std::map<QString, QPixmap> dark_images;
  bool light_theme = style_name.contains("light");

  auto* stored_images = light_theme ? &light_images : &dark_images;

  auto it = stored_images->find(filename);
  if (it == stored_images->end()) {
    QFile file(filename);
    file.open(QFile::ReadOnly | QFile::Text);
    auto svg_data = file.readAll();
    file.close();

    if (light_theme) {
      svg_data.replace("#000000", "#111111");
      svg_data.replace("#ffffff", "#dddddd");
    } else {
      svg_data.replace("#000000", "#dddddd");
      svg_data.replace("#ffffff", "#111111");
    }
    QByteArray content(svg_data);

    QSvgRenderer rr(content);
    QImage image(64, 64, QImage::Format_ARGB32);
    QPainter painter(&image);
    image.fill(Qt::transparent);
    rr.render(&painter);

    it = stored_images->insert({filename, QPixmap::fromImage(image)}).first;
  }
  return it->second;
}

}  // namespace PJ
