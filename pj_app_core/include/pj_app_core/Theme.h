#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace PJ {

// QSS source format: a PALETTE START block of `key: value` lines (one
// per line, anchored to start-of-line) followed by a PALETTE END line,
// then the body using `${KEY}` placeholders. See stylesheet_*.qss.
//
// The QApplication::setStyleSheet call lives in pj_app — pj_app_core
// must not depend on Qt6::Widgets (PJ4_PLAN.md §5.2).
class Theme : public QObject {
  Q_OBJECT
 public:
  explicit Theme(QObject* parent = nullptr);

  static QStringList availableThemes();

  QString currentTheme() const;

  QString expandedQss() const;

 public slots:
  void setTheme(const QString& name);

 signals:
  void themeChanged(const QString& name);

 private:
  void rebuildQss();

  QString name_;
  QString expanded_qss_;
};

}  // namespace PJ
