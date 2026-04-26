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
  parser.process(app);

  PJ::MainWindow window;
  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  window.show();

  return app.exec();
}
