#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <cstdlib>

#include "MainWindow.h"
#include "Style.h"
#include "WidgetTuner.h"

int main(int argc, char* argv[]) {
  // Pin to Fusion (under our Style proxy) before constructing
  // QApplication so widgets that read the style at construction time
  // don't end up with the platform's native style (KDE Breeze, GNOME
  // Adwaita, etc.) which silently overrides QSS on QMenu and other
  // popups. Style additionally suppresses default dialog-button icons
  // and the underline-mnemonic decoration.
  QApplication::setStyle(new PJ::Style(QStringLiteral("Fusion")));
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler4"));
  QApplication::setApplicationDisplayName(QStringLiteral("PlotJuggler 4"));

  // WidgetTuner: app-wide Polish-event filter that side-steps QSS
  // specificity battles by directly tagging menus and palette-painting
  // combo popups.
  auto* tuner = new PJ::WidgetTuner(&app);
  qApp->installEventFilter(tuner);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("PlotJuggler 4"));
  parser.addHelpOption();
  const QCommandLineOption test_data_option(
      QStringLiteral("test-data"), QStringLiteral("Populate the datastore with generated sin/cos samples."));
  parser.addOption(test_data_option);
  const QCommandLineOption plugin_dir_option(
      QStringLiteral("plugin-dir"),
      QStringLiteral("Override the directory where extensions are discovered and managed."), QStringLiteral("path"));
  parser.addOption(plugin_dir_option);
  parser.process(app);

  PJ::MainWindow window(parser.value(plugin_dir_option));
  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  window.show();

  return app.exec();
}
