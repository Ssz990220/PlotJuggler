// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Standalone visual demo for PJ::CheckButton (pill toggle),
// PJ::DualOptionsWidget (segmented control), and PJ::ColorPickerWidget (swatch).
// Shows both selected states and the "Override color" row layout
// (CheckButton + swatch on one line), under the real app theme.
//
// Interactive: ./build/pj_widgets/demos/pj_widgets_check_button_demo
// Screenshot : ./build/pj_widgets/demos/pj_widgets_check_button_demo --screenshot out.png [--theme dark|light]

#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "pj_widgets/CheckButton.h"
#include "pj_widgets/ColorPickerWidget.h"
#include "pj_widgets/ComboBox.h"
#include "pj_widgets/DoubleScrubber.h"
#include "pj_widgets/DualOptionsWidget.h"
#include "pj_widgets/IntScrubber.h"
#include "pj_widgets/Style.h"
#include "qss_preprocessor.h"

namespace {
using pj_widgets_demos::applyTheme;
}  // namespace

int main(int argc, char** argv) {
  QApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QApplication::setApplicationName(QStringLiteral("CheckButtonDemo"));
  QApplication app(argc, argv);
  QApplication::setStyle(new PJ::Style(QStringLiteral("Fusion")));  // same as the app: pins inputs to 20px

  QString screenshot;
  QString theme = QStringLiteral("dark");
  for (int i = 1; i < argc; ++i) {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--screenshot") && i + 1 < argc) {
      screenshot = QString::fromLocal8Bit(argv[++i]);
    } else if (arg == QStringLiteral("--theme") && i + 1 < argc) {
      theme = QString::fromLocal8Bit(argv[++i]);
    }
  }
  applyTheme(theme);

  QMainWindow win;
  win.setWindowTitle(QStringLiteral("pj_widgets compact controls"));

  auto* root = new QWidget(&win);
  root->setObjectName(QStringLiteral("ConfigPanel"));
  auto* form = new QFormLayout(root);
  form->setContentsMargins(16, 16, 16, 16);
  form->setSpacing(10);

  // Every input type that must end up the SAME compact height (20px): a native
  // line edit + our combobox + the self-painted scrubbers + the pills + swatch.
  auto* line_edit = new QLineEdit(root);
  line_edit->setText(QStringLiteral("base_link"));
  form->addRow(QStringLiteral("LineEdit:"), line_edit);
  auto* combo = new PJ::ComboBox(root);
  combo->addItems({QStringLiteral("File"), QStringLiteral("Topic"), QStringLiteral("URL")});
  form->addRow(QStringLiteral("Combo:"), combo);
  auto* dscrub = new PJ::DoubleScrubber(root);
  dscrub->setValue(0.15);
  form->addRow(QStringLiteral("DoubleScrubber:"), dscrub);
  auto* iscrub = new PJ::IntScrubber(root);
  iscrub->setValue(10);
  form->addRow(QStringLiteral("IntScrubber:"), iscrub);

  auto* off = new PJ::CheckButton(QStringLiteral("X arrow only"), root);
  form->addRow(QStringLiteral("CheckButton:"), off);
  auto* on = new PJ::CheckButton(QStringLiteral("Override color"), root);
  on->setChecked(true);

  auto* segmented_left = new PJ::DualOptionsWidget(QStringLiteral("Frame"), QStringLiteral("Arrow"), root);
  form->addRow(QStringLiteral("Segmented L:"), segmented_left);
  auto* segmented_right = new PJ::DualOptionsWidget(QStringLiteral("Frame"), QStringLiteral("Arrow"), root);
  segmented_right->setSelectedIndex(1);
  form->addRow(QStringLiteral("Segmented R:"), segmented_right);

  // "Override color" row: pill toggle + swatch side by side (Image #3 layout).
  auto* override_row = new QWidget(root);
  auto* row_layout = new QHBoxLayout(override_row);
  row_layout->setContentsMargins(0, 0, 0, 0);
  row_layout->setSpacing(8);
  auto* swatch = new PJ::ColorPickerWidget(override_row);
  swatch->setColor(QColor(0xE0, 0x39, 0x39));
  row_layout->addWidget(on);
  row_layout->addWidget(swatch);
  row_layout->addStretch();
  form->addRow(QStringLiteral("Checked + swatch:"), override_row);

  auto* toggle = new QPushButton(QStringLiteral("Toggle theme (currently: %1)").arg(theme), root);
  QObject::connect(toggle, &QPushButton::clicked, root, [toggle, theme]() mutable {
    theme = (theme == QStringLiteral("dark")) ? QStringLiteral("light") : QStringLiteral("dark");
    applyTheme(theme);
    toggle->setText(QStringLiteral("Toggle theme (currently: %1)").arg(theme));
  });
  form->addRow(QString(), toggle);

  win.setCentralWidget(root);
  win.resize(380, 300);
  win.show();

  // Print the resolved heights once laid out so the QSS can be tuned by number.
  auto report = [=]() {
    qInfo(
        "HEIGHTS  LineEdit=%d  Combo=%d  DoubleScrubber=%d  IntScrubber=%d  CheckButton=%d  DualOptions=%d  Swatch=%d",
        line_edit->height(), combo->height(), dscrub->height(), iscrub->height(), off->height(),
        segmented_left->height(), swatch->height());
  };

  if (!screenshot.isEmpty()) {
    QTimer::singleShot(300, &win, [&win, screenshot, &app, report] {
      report();
      const QPixmap pix = win.grab();
      if (pix.save(screenshot)) {
        qInfo("[check_button_demo] screenshot saved: %s (%dx%d)", qPrintable(screenshot), pix.width(), pix.height());
      } else {
        qWarning("[check_button_demo] screenshot FAILED");
      }
      app.quit();
    });
  } else {
    QTimer::singleShot(300, root, report);
  }

  return app.exec();
}
