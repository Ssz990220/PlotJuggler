// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "Theme.h"

#include <QFile>
#include <QLoggingCategory>
#include <QSettings>
#include <QStringList>
#include <map>

#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {

Q_LOGGING_CATEGORY(lcTheme, "pj.app.theme")

constexpr const char* kPaletteStartMarker = "PALETTE START";
constexpr const char* kPaletteEndMarker = "PALETTE END";

QString resourcePathFor(const QString& name) {
  return QStringLiteral(":/resources/stylesheet_%1.qss").arg(name);
}

// Expand `${KEY}` tokens against `palette`. Unknown keys are left as
// literal `${KEY}` so they're visible in the rendered styling and the
// developer notices, but we don't crash the app, since QSS files are
// resources we control and a missing key is a build-time bug.
QString expandPlaceholders(const QString& body, const std::map<QString, QString>& palette) {
  QString out;
  out.reserve(body.size());

  int i = 0;
  while (i < body.size()) {
    const int start = body.indexOf(QStringLiteral("${"), i);
    if (start < 0) {
      out.append(QStringView{body}.mid(i));
      break;
    }
    out.append(QStringView{body}.mid(i, start - i));
    const int end = body.indexOf(QLatin1Char('}'), start + 2);
    if (end < 0) {
      qCWarning(lcTheme) << "Unclosed ${...} token in QSS, leaving literal:" << body.mid(start, 32);
      out.append(QStringView{body}.mid(start));
      break;
    }
    const QString key = body.mid(start + 2, end - start - 2);
    auto it = palette.find(key);
    if (it == palette.end()) {
      qCWarning(lcTheme) << "Unknown palette key" << key << ", leaving literal placeholder";
      out.append(QStringView{body}.mid(start, end - start + 1));
    } else {
      out.append(it->second);
    }
    i = end + 1;
  }
  return out;
}

QString parseAndExpand(const QString& qss) {
  const QStringList lines = qss.split(QLatin1Char('\n'));

  std::map<QString, QString> palette;
  int i = 0;
  while (i < lines.size() && !lines[i].contains(QLatin1String(kPaletteStartMarker))) {
    ++i;
  }
  if (i == lines.size()) {
    qCWarning(lcTheme) << "No PALETTE START marker found, returning QSS unchanged";
    return qss;
  }
  ++i;  // skip the START marker line itself

  while (i < lines.size() && !lines[i].contains(QLatin1String(kPaletteEndMarker))) {
    const QString trimmed = lines[i].trimmed();
    if (!trimmed.isEmpty() && !trimmed.startsWith(QLatin1String("//"))) {
      const int colon = trimmed.indexOf(QLatin1Char(':'));
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
  if (i == lines.size()) {
    qCWarning(lcTheme) << "Reached end of QSS without PALETTE END marker, palette may be incomplete";
  } else {
    ++i;  // skip END marker
  }

  QString body;
  body.reserve(qss.size());
  for (; i < lines.size(); ++i) {
    body.append(lines[i]);
    body.append(QLatin1Char('\n'));
  }
  return expandPlaceholders(body, palette);
}

}  // namespace

QStringList Theme::availableThemes() {
  return {QStringLiteral("light"), QStringLiteral("dark")};
}

Theme::Theme(QObject* parent) : QObject(parent), name_(QSettings().value(kThemeSettingsKey, "light").toString()) {
  if (!availableThemes().contains(name_)) {
    name_ = QStringLiteral("light");
    // Rewrite stale or corrupt persisted name so subsequent readers
    // (e.g. SvgUtil::currentTheme) see the same value Theme is using.
    QSettings().setValue(kThemeSettingsKey, name_);
  }
  rebuildQss();
}

QString Theme::currentTheme() const {
  return name_;
}

QString Theme::expandedQss() const {
  return expanded_qss_;
}

void Theme::setTheme(const QString& name) {
  if (name == name_) {
    return;
  }
  if (!availableThemes().contains(name)) {
    qCWarning(lcTheme) << "Ignoring setTheme with unknown name:" << name;
    return;
  }
  name_ = name;
  QSettings().setValue(kThemeSettingsKey, name_);
  rebuildQss();
  emit themeChanged(name_);
  emit qssChanged();
}

void Theme::rebuildQss() {
  const QString path = resourcePathFor(name_);
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qCWarning(lcTheme) << "Cannot open" << path << ", stylesheet will be empty";
    expanded_qss_.clear();
    return;
  }
  const QString raw = QString::fromUtf8(file.readAll());
  expanded_qss_ = parseAndExpand(raw);
}

}  // namespace PJ
