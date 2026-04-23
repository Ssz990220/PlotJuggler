#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler 4"));
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));

  PJ::MainWindow window;
  window.show();

  return app.exec();
}
