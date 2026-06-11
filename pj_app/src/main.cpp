// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <Qt>
#include <backward.hpp>
#include <cstdlib>

#include "KeySequence.h"
#include "MainWindow.h"
#include "WidgetTuner.h"
#include "pj_widgets/Style.h"

namespace {
// One process-wide crash handler. Its constructor (run at static-init, before
// main) registers handlers for SIGSEGV/SIGABRT/SIGFPE/... that dump a
// symbolized stack trace to stderr. We instantiate it explicitly rather than
// relying on the global defined inside backward-cpp's compiled backward.cpp,
// which the linker drops from the static archive when nothing references it.
// backward-cpp recommends exactly one such instance per program.
backward::SignalHandling g_crash_handler;
}  // namespace

int main(int argc, char* argv[]) {
  // Pin to Fusion (under our Style proxy) before constructing
  // QApplication so widgets that read the style at construction time
  // don't end up with the platform's native style (KDE Breeze, GNOME
  // Adwaita, etc.) which silently overrides QSS on QMenu and other
  // popups. Style additionally suppresses default dialog-button icons
  // and the underline-mnemonic decoration.
  QApplication::setStyle(new PJ::Style(QStringLiteral("Fusion")));

  // NOTE: we deliberately do NOT set Qt::AA_ShareOpenGLContexts. It was once set
  // so a 3D scene's GL resources would survive a QOpenGLWidget context
  // recreation on ADS reparent — but it put every SceneViewWidget's context into
  // a single share group, and destroying one view's context (closing/splitting a
  // 3D dock) corrupted the VAO/FBO state of the sibling views still on screen
  // (a glBindVertexArray(non-gen name) flood + the map texture vanishing in the
  // surviving view). With each view's GL context fully independent, tearing one
  // down can no longer touch the others. The original "survive a context
  // recreation" concern is handled instead inside pj_scene3D: every render pass
  // and layer implements releaseGL(), and SceneViewWidget rebuilds its GL state
  // in initializeGL() — so a recreated context self-heals rather than relying on
  // a process-wide share group.

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler4"));
  // Until a real release-versioning scheme lands, the About box and any
  // diagnostics that embed applicationVersion() report a dev build.
  QCoreApplication::setApplicationVersion(QStringLiteral("4.0.0-dev"));
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

  // App-wide gesture watcher. Observes key presses without consuming them and
  // calls the entry point when the fixed sequence completes.
  auto* gesture_watcher =
      new PJ::KeySequenceWatcher(PJ::unlockSteps(), [&window]() { window.openEmbeddedConsole(); }, &app);
  qApp->installEventFilter(gesture_watcher);

  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  window.show();

  return app.exec();
}
