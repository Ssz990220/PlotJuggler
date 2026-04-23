#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler4"));
  QApplication::setApplicationDisplayName(QStringLiteral("PlotJuggler 4"));

  PJ::MainWindow window;
  window.show();

  return app.exec();
}
