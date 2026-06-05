#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>
#include <QString>
#include <Qt>

class QEvent;
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

  // Show or hide the title-bar close (✕) button. Hide it for dialogs whose
  // only sanctioned exits are their own action buttons (e.g. ProgressDialog).
  void setCloseButtonVisible(bool visible);

  // The body widget subclasses fill. Already in the chrome's vertical
  // layout under the title bar.
  [[nodiscard]] QWidget* contentWidget() const;
  [[nodiscard]] QLayout* contentLayout() const;

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void applyIcons();
  // Returns the edges (Qt::LeftEdge / RightEdge / TopEdge / BottomEdge,
  // or a corner combination) the point lies inside the kResizeMargin
  // band of, or 0 when the point is in the interior.
  [[nodiscard]] Qt::Edges edgesAtPoint(const QPoint& pos) const;

  Ui::Dialog* ui_;
};

}  // namespace PJ
