#pragma once

#include <QDialog>
#include <QString>

class QLayout;
class QMouseEvent;

namespace Ui {
class Dialog;
}

namespace PJ {

// Base class for app-styled dialogs: frameless window with a custom
// title bar (drag-to-move + close button) on top and an empty content
// area below. Subclasses set their title via setDialogTitle() and
// populate contentWidget() / contentLayout().
//
// Pattern:
//   class FooDialog : public Dialog {
//    public:
//     FooDialog(...) : Dialog(parent) {
//       setDialogTitle(tr("Foo"));
//       ui_->setupUi(contentWidget());  // your .ui's root is a QWidget
//     }
//   };
class Dialog : public QDialog {
  Q_OBJECT
 public:
  explicit Dialog(QWidget* parent = nullptr);
  ~Dialog() override;

  void setDialogTitle(const QString& title);
  [[nodiscard]] QString dialogTitle() const;

  // The body widget subclasses fill. Already in the chrome's vertical
  // layout under the title bar.
  [[nodiscard]] QWidget* contentWidget() const;
  [[nodiscard]] QLayout* contentLayout() const;

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  void applyIcons();

  Ui::Dialog* ui_;
};

}  // namespace PJ
