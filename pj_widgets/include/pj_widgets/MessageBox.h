#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDialog>
#include <QList>
#include <QString>
#include <initializer_list>

class QCheckBox;
class QKeyEvent;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace PJ {

// Frameless modal dialog that replaces QMessageBox with the app's visual
// language: heading + body + optional "Don't show again" checkbox +
// vertical stack of N labelled buttons. The primary button paints the
// light_purple → light_blue gradient at rest; other buttons take a solid
// surface tone keyed off their ButtonRole.
//
// Static convenience methods cover the common single-OK cases
// (information / warning / critical) plus the multi-button question.
// The instance API is for callers that need full control over labels and
// roles (matches QMessageBox::addButton).
class MessageBox : public QDialog {
  Q_OBJECT
 public:
  // Visual role applied to each button. The dialog sets this via the
  // `msgbox_role` dynamic property; the QSS keys off it.
  enum ButtonRole {
    PrimaryRole,      // Gradient at rest. The recommended affirmative action.
    NeutralRole,      // Solid neutral surface.
    DestructiveRole,  // Neutral surface with purple text; light_purple hover.
    CancelRole,       // Subtler surface. Esc / window-close return this button's index.
  };

  struct ButtonSpec {
    QString label;
    ButtonRole role;
  };

  explicit MessageBox(QWidget* parent = nullptr);
  ~MessageBox() override;

  void setTitle(const QString& title);
  void setText(const QString& text);

  // Show or hide the "Don't show again" checkbox between body and buttons.
  // Default label is "Don't show again." — override via `label` if a more
  // specific phrase is wanted.
  void setShowDontShowAgain(bool show, const QString& label = {});
  [[nodiscard]] bool dontShowAgainChecked() const;

  // Appends a button to the bottom of the vertical column. The dialog
  // owns the returned pointer.
  QPushButton* addButton(const QString& label, ButtonRole role);

  // Index of the button the user clicked (in addButton() order), or -1
  // if the dialog was rejected without picking one (Esc when no Cancel
  // role exists, or the window manager closing the dialog).
  [[nodiscard]] int clickedIndex() const;

  // --- Static convenience helpers ---
  // Single-OK informational variants — kept signature-compatible with the
  // pre-existing API so call sites in pj_app keep compiling.
  static void information(QWidget* parent, const QString& title, const QString& text);
  static void warning(QWidget* parent, const QString& title, const QString& text);
  static void critical(QWidget* parent, const QString& title, const QString& text);

  // Multi-button question. Returns the index of the clicked button (or -1
  // on reject). If `dont_show_again` is non-null, the checkbox is shown
  // above the buttons and its checked state is written back through it.
  static int question(
      QWidget* parent, const QString& title, const QString& text, std::initializer_list<ButtonSpec> buttons,
      bool* dont_show_again = nullptr);

 protected:
  void keyPressEvent(QKeyEvent* event) override;

 private:
  QLabel* title_label_;
  QLabel* body_label_;
  QCheckBox* dont_show_again_;
  QVBoxLayout* button_column_;
  QList<QPushButton*> buttons_;
  QList<ButtonRole> button_roles_;
  int clicked_index_ = -1;
};

}  // namespace PJ
