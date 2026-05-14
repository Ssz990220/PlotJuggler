#include "MessageBox.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Dialog.h"

namespace PJ {

void MessageBox::information(QWidget* parent, const QString& title, const QString& text) {
  show(parent, title, text);
}

void MessageBox::warning(QWidget* parent, const QString& title, const QString& text) {
  show(parent, title, text);
}

void MessageBox::critical(QWidget* parent, const QString& title, const QString& text) {
  show(parent, title, text);
}

void MessageBox::show(QWidget* parent, const QString& title, const QString& text) {
  Dialog dlg(parent);
  dlg.setDialogTitle(title);
  dlg.setMinimumSize(420, 160);

  auto* body = new QWidget;
  auto* layout = new QVBoxLayout(body);
  layout->setContentsMargins(20, 16, 20, 16);
  layout->setSpacing(16);

  auto* label = new QLabel(text, body);
  label->setWordWrap(true);
  label->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  layout->addWidget(label, 1);

  auto* footer = new QHBoxLayout;
  footer->addStretch();
  auto* ok_btn = new QPushButton(tr("OK"), body);
  ok_btn->setDefault(true);
  ok_btn->setAutoDefault(true);
  QObject::connect(ok_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
  footer->addWidget(ok_btn);
  layout->addLayout(footer);

  dlg.contentLayout()->addWidget(body);
  dlg.exec();
}

}  // namespace PJ
