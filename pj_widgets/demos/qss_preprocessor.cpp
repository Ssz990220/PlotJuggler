// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "qss_preprocessor.h"

#include <QApplication>
#include <QChar>
#include <QDebug>
#include <QFile>
#include <QIODevice>
#include <QSettings>
#include <QStringList>
#include <QStringView>
#include <map>

namespace pj_widgets_demos {

namespace {

QString ExpandPlaceholders(const QString& body, const std::map<QString, QString>& palette) {
  QString out;
  out.reserve(body.size());
  qsizetype i = 0;
  while (i < body.size()) {
    const qsizetype start = body.indexOf(QStringLiteral("${"), i);
    if (start < 0) {
      out.append(QStringView{body}.mid(i));
      break;
    }
    out.append(QStringView{body}.mid(i, start - i));
    const qsizetype end = body.indexOf(QLatin1Char('}'), start + 2);
    if (end < 0) {
      out.append(QStringView{body}.mid(start));
      break;
    }
    const QString key = body.mid(start + 2, end - start - 2);
    auto it = palette.find(key);
    if (it == palette.end()) {
      qWarning() << "Unknown palette key" << key;
      out.append(QStringView{body}.mid(start, end - start + 1));
    } else {
      out.append(it->second);
    }
    i = end + 1;
  }
  return out;
}

}  // namespace

QString LoadAndExpandQss(const QString& theme) {
  const QString path = QStringLiteral("%1/stylesheet_%2.qss").arg(QStringLiteral(PJ_QSS_DIR), theme);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Cannot open" << path;
    return {};
  }
  const QString raw = QString::fromUtf8(file.readAll());
  const QStringList lines = raw.split(QLatin1Char('\n'));

  std::map<QString, QString> palette;
  int i = 0;
  while (i < lines.size() && !lines[i].contains(QLatin1String("PALETTE START"))) {
    ++i;
  }
  ++i;
  while (i < lines.size() && !lines[i].contains(QLatin1String("PALETTE END"))) {
    const QString trimmed = lines[i].trimmed();
    if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1String("//"))) {
      const qsizetype colon = trimmed.indexOf(QLatin1Char(':'));
      if (colon > 0) {
        QString key = trimmed.left(colon).trimmed();
        QString value = trimmed.mid(colon + 1).trimmed();
        if (value.endsWith(QLatin1Char(';'))) {
          value.chop(1);
          value = value.trimmed();
        }
        palette.emplace(std::move(key), std::move(value));
      }
    }
    ++i;
  }
  ++i;

  QString body;
  for (; i < lines.size(); ++i) {
    body.append(lines[i]);
    body.append(QLatin1Char('\n'));
  }
  return ExpandPlaceholders(body, palette);
}

void ApplyTheme(const QString& theme) {
  qApp->setStyleSheet(LoadAndExpandQss(theme));
  QSettings().setValue(QStringLiteral("StyleSheet::theme"), theme);
}

}  // namespace pj_widgets_demos
