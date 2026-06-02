// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for pj_widgets/MessageBox.
//
// Interactive window with one button per MessageBox variant
// (information / warning / critical / question / destructive) plus a
// theme toggle.

#include <QApplication>
#include <QDebug>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/MessageBox.h"
#include "qss_preprocessor.h"

namespace {

using pj_widgets_demos::ApplyTheme;

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QApplication::setApplicationName(QStringLiteral("MessageBoxDemo"));

  QString interactive_theme = QStringLiteral("dark");
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == QStringLiteral("--theme") && i + 1 < argc) {
      interactive_theme = QString::fromLocal8Bit(argv[++i]);
    }
  }

  ApplyTheme(interactive_theme);

  QMainWindow win;
  win.setWindowTitle(QStringLiteral("PJ MessageBox demo"));
  auto* central = new QWidget;
  auto* lay = new QVBoxLayout(central);
  lay->setContentsMargins(16, 16, 16, 16);
  lay->setSpacing(8);

  auto add_trigger = [&](const QString& label, std::function<void()> on_click) {
    auto* btn = new QPushButton(label, central);
    QObject::connect(btn, &QPushButton::clicked, central, on_click);
    lay->addWidget(btn);
  };

  add_trigger(QStringLiteral("information() — single OK"), [&win]() {
    PJ::MessageBox::information(
        &win, QStringLiteral("Layout saved"), QStringLiteral("Workspace layout written to disk."));
  });
  add_trigger(QStringLiteral("warning() — single OK"), [&win]() {
    PJ::MessageBox::warning(
        &win, QStringLiteral("Load failed"), QStringLiteral("Cannot read /tmp/missing.csv — file does not exist."));
  });
  add_trigger(QStringLiteral("critical() — single OK"), [&win]() {
    PJ::MessageBox::critical(
        &win, QStringLiteral("Fatal error"), QStringLiteral("Internal pipeline crashed; restart required."));
  });
  add_trigger(QStringLiteral("question() — 3 vertical buttons (canonical)"), [&win]() {
    bool dont_show = false;
    const int chosen = PJ::MessageBox::question(
        &win, QStringLiteral("Close pane?"), QStringLiteral("You have 1 process running in this pane."),
        {{QStringLiteral("Yes, close"), PJ::MessageBox::PrimaryRole},
         {QStringLiteral("Show running processes"), PJ::MessageBox::NeutralRole},
         {QStringLiteral("Cancel"), PJ::MessageBox::CancelRole}},
        &dont_show);
    qInfo() << "chosen=" << chosen << "dont_show_again=" << dont_show;
  });
  add_trigger(QStringLiteral("question() — destructive role"), [&win]() {
    const int chosen = PJ::MessageBox::question(
        &win, QStringLiteral("Delete dataset?"),
        QStringLiteral("This will permanently remove 'experiment_42.bag' and its derived series."),
        {{QStringLiteral("Delete"), PJ::MessageBox::DestructiveRole},
         {QStringLiteral("Cancel"), PJ::MessageBox::CancelRole}});
    qInfo() << "chosen=" << chosen;
  });

  auto* toggle = new QPushButton(QStringLiteral("Toggle theme (currently: %1)").arg(interactive_theme), central);
  QObject::connect(toggle, &QPushButton::clicked, central, [toggle, theme = interactive_theme]() mutable {
    theme = (theme == QStringLiteral("dark")) ? QStringLiteral("light") : QStringLiteral("dark");
    ApplyTheme(theme);
    toggle->setText(QStringLiteral("Toggle theme (currently: %1)").arg(theme));
  });
  lay->addWidget(toggle);
  lay->addStretch();

  win.setCentralWidget(central);
  win.resize(360, 280);
  win.show();
  return app.exec();
}
