#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <cstdlib>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler4"));
  QApplication::setApplicationDisplayName(QStringLiteral("PlotJuggler 4"));

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
