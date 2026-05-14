#pragma once

#include <QObject>
#include <QString>

class QWidget;

namespace PJ {

// App-styled replacement for QMessageBox::information / warning / critical.
// Each call opens a modal Dialog with a level icon, the message text,
// and an OK button. No icon variation across levels for now — same chrome,
// consistent with the rest of the app.
class MessageBox : public QObject {
  Q_OBJECT
 public:
  static void information(QWidget* parent, const QString& title, const QString& text);
  static void warning(QWidget* parent, const QString& title, const QString& text);
  static void critical(QWidget* parent, const QString& title, const QString& text);

 private:
  static void show(QWidget* parent, const QString& title, const QString& text);
};

}  // namespace PJ
