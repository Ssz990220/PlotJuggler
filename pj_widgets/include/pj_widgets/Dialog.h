#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>
#include <QString>
#include <Qt>

#include "pj_widgets/ChromeMetrics.h"

class QEvent;
class QLayout;
class QMouseEvent;
class QShowEvent;

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

  // Size the title-bar chrome (height, close-button extent, icon size, padding)
  // from the app's shared ChromeMetrics, so this dialog's chrome matches the main
  // window exactly. Called with defaults in the constructor; hosts that know the
  // live metrics (the dialog host, app) call it again with the current values.
  void setChromeMetrics(const ChromeMetrics& metrics);

  // The body widget subclasses fill. Already in the chrome's vertical
  // layout under the title bar.
  [[nodiscard]] QWidget* contentWidget() const;
  [[nodiscard]] QLayout* contentLayout() const;

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  // On first show, give the content's scroll areas the canonical overlay pill
  // scrollbars (via attachPillScrollbars) so every app-styled dialog scrolls
  // with app-styled bars — no per-dialog wiring. First show, not construction,
  // because subclasses populate contentWidget() in their own constructor body
  // after Dialog's runs.
  void showEvent(QShowEvent* event) override;

 private:
  void applyIcons();
  // Returns the edges (Qt::LeftEdge / RightEdge / TopEdge / BottomEdge,
  // or a corner combination) the point lies inside the kResizeMargin
  // band of, or 0 when the point is in the interior.
  [[nodiscard]] Qt::Edges edgesAtPoint(const QPoint& pos) const;

  Ui::Dialog* ui_;
  // One-shot guard so the first-show pill attach runs once (attach itself is
  // idempotent, but this avoids re-walking the tree on every show).
  bool scroll_pills_attached_ = false;
};

}  // namespace PJ
