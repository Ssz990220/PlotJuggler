// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/MessageBox.h"

#include <QCheckBox>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace PJ {

namespace {

// Maps the C++ enum to the string the QSS selectors key off.
const char* RoleToToken(MessageBox::ButtonRole role) {
  switch (role) {
    case MessageBox::PrimaryRole:
      return "primary";
    case MessageBox::NeutralRole:
      return "neutral";
    case MessageBox::DestructiveRole:
      return "destructive";
    case MessageBox::CancelRole:
      return "cancel";
  }
  return "neutral";
}

}  // namespace

MessageBox::MessageBox(QWidget* parent) : QDialog(parent) {
  setObjectName(QStringLiteral("pjMessageBox"));

  // Frameless modal so the WM's title-bar chrome doesn't fight the in-body
  // heading. WA_StyledBackground lets the QDialog#pjMessageBox QSS rule
  // actually paint the dark/light surface and the 1-px border.
  setWindowFlag(Qt::FramelessWindowHint, true);
  setWindowFlag(Qt::Dialog, true);
  // ARGB surface so the corner pixels outside the inner card's rounded
  // background can be alpha=0. The card itself paints opaque, so text and
  // children never composite against a partly-transparent surface — only
  // the four corner pixels do, which is what gives the dialog its rounded
  // shape on compositors that ignore setMask (i.e. most Wayland setups).
  setAttribute(Qt::WA_TranslucentBackground, true);
  setModal(true);

  // Width contract. Word-wrapped QLabels report an arbitrarily small minimum
  // width (height grows via heightForWidth instead), so without these the
  // layout collapses to fit the narrowest widget and the body text wraps
  // into a thin strip. 320 reads comfortably for the short messages in
  // information()/warning()/critical(); 460 caps growth for verbose bodies
  // so a paragraph wraps onto a few lines rather than stretching across the
  // screen. Picked by eye against the info_dialog.png reference.
  setMinimumWidth(320);
  setMaximumWidth(460);

  // Outer layout: just hosts the card; no margins so the card fills the
  // window flush and its rounded corners are the dialog's only shape.
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto* card = new QFrame(this);
  card->setObjectName(QStringLiteral("pjMessageBoxCard"));
  outer->addWidget(card);

  // Inner layout on the card. Uniform spacing throughout so every gap reads
  // the same: title→body, body→checkbox, body→buttons (when no checkbox),
  // checkbox→buttons — every adjacent visible pair sits 14 px apart.
  // Earlier versions added a 6 px sub-section spacer above the buttons,
  // which made the spacing inconsistent when the checkbox was hidden.
  auto* root = new QVBoxLayout(card);
  root->setContentsMargins(24, 24, 24, 24);
  root->setSpacing(14);

  title_label_ = new QLabel(card);
  title_label_->setObjectName(QStringLiteral("pjMessageBoxTitle"));
  title_label_->setWordWrap(true);
  root->addWidget(title_label_);

  body_label_ = new QLabel(card);
  body_label_->setObjectName(QStringLiteral("pjMessageBoxBody"));
  body_label_->setWordWrap(true);
  body_label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  root->addWidget(body_label_);

  dont_show_again_ = new QCheckBox(card);
  dont_show_again_->setObjectName(QStringLiteral("pjMessageBoxDontShowAgain"));
  dont_show_again_->setText(tr("Don't show again."));
  dont_show_again_->hide();  // opt-in via setShowDontShowAgain()
  root->addWidget(dont_show_again_);

  auto* btn_holder = new QVBoxLayout;
  btn_holder->setContentsMargins(0, 0, 0, 0);
  // Visible gap between stacked buttons so each reads as its own affordance.
  // 8 px reads as buttons touching against the ~38 px button height; 14 px
  // matches the root spacing so the rhythm is uniform top to bottom.
  btn_holder->setSpacing(14);
  button_column_ = btn_holder;
  root->addLayout(btn_holder);
}

MessageBox::~MessageBox() = default;

void MessageBox::setTitle(const QString& title) {
  title_label_->setText(title);
  setWindowTitle(title);
}

void MessageBox::setText(const QString& text) {
  body_label_->setText(text);
}

void MessageBox::setShowDontShowAgain(bool show, const QString& label) {
  dont_show_again_->setVisible(show);
  if (!label.isEmpty()) {
    dont_show_again_->setText(label);
  }
}

bool MessageBox::dontShowAgainChecked() const {
  return dont_show_again_->isVisible() && dont_show_again_->isChecked();
}

QPushButton* MessageBox::addButton(const QString& label, ButtonRole role) {
  auto* btn = new QPushButton(label, this);
  btn->setObjectName(QStringLiteral("pjMessageBoxButton"));
  btn->setProperty("msgbox_role", QLatin1String(RoleToToken(role)));
  // Disable autoDefault so Enter doesn't trigger a non-primary button just
  // because focus traversed onto it. The primary button still receives
  // default-button focus.
  btn->setAutoDefault(false);
  btn->setDefault(role == PrimaryRole);

  const int index = static_cast<int>(buttons_.size());
  buttons_.append(btn);
  button_roles_.append(role);
  button_column_->addWidget(btn);

  QObject::connect(btn, &QPushButton::clicked, this, [this, index]() {
    clicked_index_ = index;
    accept();
  });
  return btn;
}

int MessageBox::clickedIndex() const {
  return clicked_index_;
}

void MessageBox::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape) {
    // Map Esc to the first CancelRole button if one is present, so callers
    // that supply a Cancel button get its index returned. Otherwise reject
    // with clicked_index_ = -1.
    for (int i = 0; i < button_roles_.size(); ++i) {
      if (button_roles_[i] == CancelRole) {
        clicked_index_ = i;
        accept();
        return;
      }
    }
    reject();
    return;
  }
  QDialog::keyPressEvent(event);
}

// --- Static helpers ----------------------------------------------------------

void MessageBox::information(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), PrimaryRole);
  dlg.exec();
}

void MessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), PrimaryRole);
  dlg.exec();
}

void MessageBox::critical(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), PrimaryRole);
  dlg.exec();
}

int MessageBox::question(
    QWidget* parent, const QString& title, const QString& text, std::initializer_list<ButtonSpec> buttons,
    bool* dont_show_again) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  if (dont_show_again != nullptr) {
    dlg.setShowDontShowAgain(true);
  }
  for (const auto& spec : buttons) {
    dlg.addButton(spec.label, spec.role);
  }
  dlg.exec();
  if (dont_show_again != nullptr) {
    *dont_show_again = dlg.dontShowAgainChecked();
  }
  return dlg.clickedIndex();
}

}  // namespace PJ
