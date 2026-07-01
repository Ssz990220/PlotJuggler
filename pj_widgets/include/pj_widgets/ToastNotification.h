#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QFrame>
#include <QLabel>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QString>
#include <QTimer>

class QHBoxLayout;

namespace PJ {

// A single transient "toast": a rounded frame that slides in from the right,
// shows an optional left-hand icon plus a rich-text message, and auto-dismisses
// after `timeout_ms` (or when the user clicks the ✕). Positioning and stacking
// are driven by ToastManager — a ToastNotification never places itself.
//
// The message label is rich-text with external links enabled, so an
// `<a href>` in the message opens in the system browser with no extra wiring.
class ToastNotification : public QFrame {
  Q_OBJECT

 public:
  // `icon` is optional; when non-null it is scaled to kIconSize and shown on
  // the left. `timeout_ms == 0` disables auto-dismiss (the toast stays until
  // closed). The timeout countdown only starts once the slide-in finishes.
  // In practice the timeout is always supplied by ToastManager::showToast; the
  // default only applies to a directly-constructed toast.
  explicit ToastNotification(
      const QString& message, const QPixmap& icon = QPixmap(), int timeout_ms = 8000, QWidget* parent = nullptr);

  ~ToastNotification() override;

  // Slide in from the right to the position ToastManager has already moved it to.
  void showAnimated();

  // Slide out to the right, then emit closed() and deleteLater().
  void hideAnimated();

  QString message() const;
  void setMessage(const QString& message);

  // Scales to kIconSize with the left corners rounded; a null pixmap hides it.
  void setIcon(const QPixmap& icon);

  // Retarget the slide destination, re-aiming an in-flight animation if needed.
  void updateTargetPosition(const QPoint& target);

 signals:
  // Emitted once the close animation has fully finished.
  void closed();

 private slots:
  void onCloseClicked();
  void onAnimationFinished();
  void onTimeout();

 private:
  void setupUI();
  void setupAnimation();
  // Clips `source` with only its left corners rounded (the right side abuts the
  // message area), returning a transparent-padded pixmap.
  QPixmap createRoundedPixmap(const QPixmap& source, int radius);

  QHBoxLayout* layout_ = nullptr;
  QLabel* icon_label_ = nullptr;
  QLabel* message_label_ = nullptr;
  QPushButton* close_button_ = nullptr;
  QPropertyAnimation* slide_animation_ = nullptr;
  QTimer* timeout_timer_ = nullptr;

  int timeout_ms_ = 0;
  bool is_closing_ = false;

  static constexpr int kBorderRadius = 6;
  static constexpr int kIconSize = 128;
  static constexpr int kAnimationDurationMs = 300;
};

}  // namespace PJ
