#pragma once

#include <QEvent>
#include <QLineEdit>
#include <QPalette>

namespace PJ {

// QLineEdit subclass that fixes placeholder-text colour when a stylesheet
// is applied (Qt bug QTBUG-92199).
class LineEdit : public QLineEdit {
  Q_OBJECT
 public:
  explicit LineEdit(QWidget* parent = nullptr) : QLineEdit(parent) { updatePlaceholderColor(); }

  explicit LineEdit(const QString& text, QWidget* parent = nullptr) : QLineEdit(text, parent) {
    updatePlaceholderColor();
  }

 protected:
  void changeEvent(QEvent* event) override {
    QLineEdit::changeEvent(event);
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
      updatePlaceholderColor();
    }
  }

 private:
  void updatePlaceholderColor() {
    QPalette pal = palette();
    const QColor text_color = pal.color(QPalette::Text);
    const QColor base_color = pal.color(QPalette::Base);
    QColor placeholder;
    placeholder.setRed((text_color.red() + base_color.red()) / 2);
    placeholder.setGreen((text_color.green() + base_color.green()) / 2);
    placeholder.setBlue((text_color.blue() + base_color.blue()) / 2);
    pal.setColor(QPalette::PlaceholderText, placeholder);
    setPalette(pal);
  }
};

}  // namespace PJ
