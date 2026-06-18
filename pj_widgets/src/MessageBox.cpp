// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/MessageBox.h"

#include <QCheckBox>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace PJ {

namespace {

// Maps the C++ enum to the string the QSS selectors key off.
const char* roleToToken(MessageBox::ButtonRole role) {
  switch (role) {
    case MessageBox::kPrimaryRole:
      return "primary";
    case MessageBox::kNeutralRole:
      return "neutral";
    case MessageBox::kDestructiveRole:
      return "destructive";
    case MessageBox::kCancelRole:
      return "cancel";
  }
  return "neutral";
}

// Layout insets used to derive the per-line wrap budget for button labels.
// They mirror values set elsewhere — the card content margin in the ctor
// (root->setContentsMargins(24, …)), the 1px card border and the button's
// horizontal padding from the QSS rule for #pjMessageBoxButton (padding:
// 6px 12px). They only need to be *conservative*: under-estimating the budget
// wraps a hair early but never lets a line get clipped, so small drift in the
// QSS values is harmless.
constexpr int kCardContentMargin = 24;
constexpr int kCardBorder = 1;
constexpr int kButtonHPadding = 12;
constexpr int kWrapSlack = 4;  // extra safety against font-metric rounding

// Greedy word-wrap: insert '\n' so no single rendered line exceeds max_width.
// Returns text unchanged when it already fits on one line (the common case for
// short labels, which then render exactly as before). A single word wider than
// max_width is kept on its own line rather than dropped or split mid-word.
QString wrapLabelToWidth(const QString& text, const QFontMetrics& fm, int max_width) {
  if (max_width <= 0 || fm.horizontalAdvance(text) <= max_width) {
    return text;
  }
  const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  if (words.isEmpty()) {
    return text;
  }
  // Greedily pack words onto the current line; start a new line when the next
  // word would overflow. A single word wider than max_width still gets its own
  // line (it just can't be packed with anything).
  QStringList lines;
  QString line = words.first();
  for (int i = 1; i < words.size(); ++i) {
    const QString candidate = line + QLatin1Char(' ') + words[i];
    if (fm.horizontalAdvance(candidate) <= max_width) {
      line = candidate;
    } else {
      lines.append(line);
      line = words[i];
    }
  }
  lines.append(line);
  return lines.join(QLatin1Char('\n'));
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
  // Fixed 6 px gap between stacked buttons — tighter than the 14 px body→button
  // gap so the buttons read as one grouped affordance. This value must render
  // identically in every dialog; showEvent() resizes the dialog to the exact
  // height its content needs so the layout can never compress this spacing
  // (see the comment there).
  btn_holder->setSpacing(6);
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
  btn->setProperty("msgbox_role", QLatin1String(roleToToken(role)));
  // Disable autoDefault so Enter doesn't trigger a non-primary button just
  // because focus traversed onto it. The primary button still receives
  // default-button focus.
  btn->setAutoDefault(false);
  btn->setDefault(role == kPrimaryRole);

  const int index = static_cast<int>(buttons_.size());
  buttons_.append(btn);
  button_roles_.append(role);
  button_labels_.append(label);  // un-wrapped original; see rewrapButtonLabels()
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
      if (button_roles_[i] == kCancelRole) {
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

bool MessageBox::event(QEvent* event) {
  // Wrap long button labels on Polish: by then QStyleSheetStyle has applied the
  // QSS font (so QFontMetrics are accurate), and Polish runs before the dialog
  // auto-sizes. Wrapping in showEvent() would be too late — the dialog would
  // already be sized for single-line buttons and clip the wrapped text.
  if (event->type() == QEvent::Polish) {
    const bool handled = QDialog::event(event);  // let the style apply the font first
    rewrapButtonLabels();
    return handled;
  }
  return QDialog::event(event);
}

void MessageBox::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  // The QVBoxLayout's preferred height is computed using the body label's
  // heightForWidth at the dialog's *sizeHint* width, but a word-wrapped body
  // needs more height at the (narrower) width the dialog is actually shown at.
  // Left uncorrected, the layout is over-constrained and steals the deficit
  // from the most compressible items — the inter-button spacings — so the gaps
  // shrink by a different amount in every dialog. Resize to the exact height
  // the content needs at the real width so every spacing renders at its set
  // value (6 px between buttons, 14 px elsewhere).
  if (layout() != nullptr && layout()->hasHeightForWidth()) {
    const int needed = layout()->heightForWidth(width());
    if (needed > 0 && needed != height()) {
      resize(width(), needed);
    }
  }
}

void MessageBox::rewrapButtonLabels() {
  // Widest a single button line may be before the dialog would exceed its
  // maximumWidth(): the cap minus the card margins, border and button padding.
  const int max_line = maximumWidth() - 2 * kCardContentMargin - 2 * kCardBorder - 2 * kButtonHPadding - kWrapSlack;
  for (int i = 0; i < buttons_.size(); ++i) {
    // Force the button's own polish so its font reflects the QSS font-size
    // before we measure: the dialog's Polish fires before its children's, so
    // without this we'd wrap against the default font and overflow at the
    // larger rendered size.
    buttons_[i]->ensurePolished();
    buttons_[i]->setText(wrapLabelToWidth(button_labels_[i], QFontMetrics(buttons_[i]->font()), max_line));
  }
}

// --- Static helpers ----------------------------------------------------------

void MessageBox::information(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), kPrimaryRole);
  dlg.exec();
}

void MessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), kPrimaryRole);
  dlg.exec();
}

void MessageBox::critical(QWidget* parent, const QString& title, const QString& text) {
  MessageBox dlg(parent);
  dlg.setTitle(title);
  dlg.setText(text);
  dlg.addButton(tr("OK"), kPrimaryRole);
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
