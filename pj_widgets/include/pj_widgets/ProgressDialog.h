#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_widgets/Dialog.h"

class QKeyEvent;
class QLabel;
class QProgressBar;
class QPushButton;
class QVBoxLayout;

namespace PJ {

// App-styled modal progress dialog with up to two "stop" buttons, built on the
// frameless Dialog chrome so it matches MessageBox / FileDialog instead of the
// native QProgressDialog look.
//
// Domain-neutral on purpose: it reports WHICH button was pressed
// (Action::Primary / Action::Secondary) and leaves the meaning to the caller.
// FileLoader, for instance, maps Primary -> keep-partial and Secondary ->
// discard; the dialog itself knows nothing about datasets, flushing, or any
// feature policy.
//
// Driving model: the owning code runs its work synchronously on the GUI thread
// and pumps the event loop (QCoreApplication::processEvents) between progress
// steps, polling action() to learn whether the user asked to stop.
// stopRequested() is also emitted for callers that prefer a signal. The chosen
// action is sticky until resetAction(); completion paths just hide() the
// dialog (there is no QProgressDialog-style reset()).
//
// No close affordance: the base ✕ is hidden and Esc is swallowed, so the two
// action buttons are the only sanctioned exits — matching the intent of a
// progress prompt that must not be dismissed mid-operation.
class ProgressDialog : public Dialog {
  Q_OBJECT
 public:
  // Which stop button the user pressed, or None if neither was (the operation
  // ran to completion).
  enum class Action { None, Primary, Secondary };

  explicit ProgressDialog(QWidget* parent = nullptr);
  ~ProgressDialog() override;

  // Configure a stop button. An empty `label` hides it. `icon_path` is an SVG
  // resource path, recolored for the active theme (empty = no icon). Primary
  // sits left of Secondary.
  void setPrimaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});
  void setSecondaryButton(const QString& label, const QString& icon_path = {}, const QString& tooltip = {});

  // Progress-bar range and value. setRange(0, 0) yields an indeterminate
  // (busy) bar, matching QProgressBar semantics.
  void setRange(int minimum, int maximum);
  void setValue(int value);
  [[nodiscard]] int maximum() const;

  // Body message shown above the bar. The titlebar text is set separately via
  // the base Dialog::setDialogTitle(). An empty string collapses the row.
  void setMessage(const QString& text);

  // Show or hide both stop buttons together — e.g. a non-cancellable phase. A
  // button with no configured label stays hidden regardless.
  void setStopButtonsVisible(bool visible);

  // The button pressed so far this run; sticky until resetAction().
  [[nodiscard]] Action action() const {
    return action_;
  }
  void resetAction() {
    action_ = Action::None;
  }

 signals:
  // Emitted once when the user presses a stop button (Primary or Secondary).
  void stopRequested(Action action);

 protected:
  void keyPressEvent(QKeyEvent* event) override;

 private:
  void configureButton(QPushButton* button, const QString& label, const QString& icon_path, const QString& tooltip);

  QLabel* message_ = nullptr;
  QProgressBar* bar_ = nullptr;
  QPushButton* primary_ = nullptr;
  QPushButton* secondary_ = nullptr;
  Action action_ = Action::None;
};

}  // namespace PJ
