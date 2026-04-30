#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace PJ {

// Theme service: parses templated QSS, exposes the expanded stylesheet,
// persists the chosen theme name, and emits themeChanged() on switch.
//
// Lives in pj_app_core so it can be reused by Qt-Core/Gui consumers
// (preferences-dialog model, tests, future settings UI). The actual
// QApplication::setStyleSheet call lives in pj_app — pj_app_core does
// not depend on Qt6::Widgets.
//
// The QSS source files live at :/resources/stylesheet_<name>.qss and
// open with a `PALETTE START` block of `key: value` lines (one per
// line, anchored to the start of the line). The body uses `${KEY}`
// tokens that get substituted by the parser.
class Theme : public QObject {
  Q_OBJECT
 public:
  explicit Theme(QObject* parent = nullptr);

  // Available theme names (currently {"light", "dark"}). Stable order,
  // suitable for combo-box population.
  static QStringList availableThemes();

  QString currentTheme() const;

  // Expanded QSS for the current theme — palette substituted, ready to
  // hand to QApplication::setStyleSheet. Cached; rebuilt on setTheme().
  // Empty on parse failure (logged via qWarning).
  QString expandedQss() const;

 public slots:
  // Switch to the named theme. No-op if same as current. On success:
  // persists to QSettings, rebuilds expandedQss(), emits themeChanged().
  void setTheme(const QString& name);

 signals:
  void themeChanged(const QString& name);

 private:
  void rebuildQss();

  QString name_;
  QString expanded_qss_;
};

}  // namespace PJ
