// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for QComboBox styling.
//
// Interactive window with several PJ::ComboBox variants (basic, long
// content, many items, editable, disabled) plus a DoubleScrubber for
// side-by-side input-chrome comparison and a theme toggle. Click a
// combobox to see the popup styling.

#include <QApplication>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "qss_preprocessor.h"

namespace {

using pj_widgets_demos::ApplyTheme;

// Populates a fresh PJ::ComboBox with a labelled variant. Caller owns.
QComboBox* MakeCombo(const QString& variant, QWidget* parent) {
  auto* combo = new PJ::ComboBox(parent);
  if (variant == QStringLiteral("basic")) {
    combo->addItems(
        {QStringLiteral("Apple"), QStringLiteral("Banana"), QStringLiteral("Cherry"), QStringLiteral("Date"),
         QStringLiteral("Elderberry")});
  } else if (variant == QStringLiteral("long")) {
    combo->addItems(
        {QStringLiteral("Short"), QStringLiteral("Medium length item"),
         QStringLiteral("A very long item name that exceeds the typical combobox width"),
         QStringLiteral("Another quite long entry for measuring elision behaviour")});
  } else if (variant == QStringLiteral("many")) {
    for (int i = 1; i <= 30; ++i) {
      combo->addItem(QStringLiteral("Item %1").arg(i));
    }
  } else if (variant == QStringLiteral("editable")) {
    combo->setEditable(true);
    combo->addItems(
        {QStringLiteral("Recent value 1"), QStringLiteral("Recent value 2"), QStringLiteral("Recent value 3")});
  } else if (variant == QStringLiteral("disabled")) {
    combo->addItems({QStringLiteral("This is disabled"), QStringLiteral("Option B")});
    combo->setEnabled(false);
  }
  return combo;
}

QWidget* MakeRow(const QString& label, const QString& variant, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* lay = new QFormLayout(row);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setHorizontalSpacing(12);
  auto* lbl = new QLabel(label, row);
  lay->addRow(lbl, MakeCombo(variant, row));
  return row;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QApplication::setApplicationName(QStringLiteral("ComboBoxDemo"));

  QString interactive_theme = QStringLiteral("dark");
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == QStringLiteral("--theme") && i + 1 < argc) {
      interactive_theme = QString::fromLocal8Bit(argv[++i]);
    }
  }

  ApplyTheme(interactive_theme);

  QMainWindow win;
  win.setWindowTitle(QStringLiteral("PJ QComboBox demo"));
  auto* central = new QWidget;
  auto* lay = new QVBoxLayout(central);
  lay->setContentsMargins(20, 20, 20, 20);
  lay->setSpacing(12);

  lay->addWidget(MakeRow(QStringLiteral("Basic"), QStringLiteral("basic"), central));
  lay->addWidget(MakeRow(QStringLiteral("Long content"), QStringLiteral("long"), central));
  lay->addWidget(MakeRow(QStringLiteral("Many items"), QStringLiteral("many"), central));
  lay->addWidget(MakeRow(QStringLiteral("Editable"), QStringLiteral("editable"), central));
  lay->addWidget(MakeRow(QStringLiteral("Disabled"), QStringLiteral("disabled"), central));

  // Reference: a DoubleScrubber for side-by-side comparison of the closed
  // input chrome — the QComboBox above should look visually consistent.
  {
    auto* ref_row = new QWidget(central);
    auto* ref_lay = new QFormLayout(ref_row);
    ref_lay->setContentsMargins(0, 0, 0, 0);
    ref_lay->setHorizontalSpacing(12);
    auto* scrubber = new PJ::DoubleScrubber(ref_row);
    scrubber->setRange(0.0, 100.0);
    scrubber->setValue(42.0);
    ref_lay->addRow(new QLabel(QStringLiteral("DoubleScrubber (reference)"), ref_row), scrubber);
    lay->addWidget(ref_row);
  }

  auto* toggle = new QPushButton(QStringLiteral("Toggle theme (currently: %1)").arg(interactive_theme), central);
  QObject::connect(toggle, &QPushButton::clicked, central, [toggle, theme = interactive_theme]() mutable {
    theme = (theme == QStringLiteral("dark")) ? QStringLiteral("light") : QStringLiteral("dark");
    ApplyTheme(theme);
    toggle->setText(QStringLiteral("Toggle theme (currently: %1)").arg(theme));
  });
  lay->addWidget(toggle);
  lay->addStretch();

  win.setCentralWidget(central);
  win.resize(480, 380);
  win.show();

  return app.exec();
}
