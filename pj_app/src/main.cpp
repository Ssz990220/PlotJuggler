// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QTimer>
#include <Qt>
#include <backward.hpp>
#include <cstdlib>

#include "DebugMode.h"
#include "KeySequence.h"
#include "MainWindow.h"
#include "WidgetTuner.h"
#include "pj_plotting/PlotWidgetBase.h"
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
  const QCommandLineOption layout_option(
      QStringLiteral("layout"), QStringLiteral("Load a layout file on startup, reloading its data source(s)."),
      QStringLiteral("path"));
  parser.addOption(layout_option);
  const QCommandLineOption autoplay_option(
      QStringLiteral("autoplay"), QStringLiteral(
                                      "Start looping playback automatically once a data source provides a time range "
                                      "(useful with --layout / --test-data for demos and profiling)."));
  parser.addOption(autoplay_option);
  const QCommandLineOption debug_mode_option(
      QStringLiteral("debug-mode"),
      QStringLiteral("Reveal developer-only preferences and tooling that are hidden in normal runs."));
  parser.addOption(debug_mode_option);
  const QCommandLineOption disable_opengl_option(
      QStringLiteral("disable-opengl"),
      QStringLiteral(
          "Force plots onto the software raster canvas for this session, overriding the saved OpenGL "
          "preference (does not change it)."));
  parser.addOption(disable_opengl_option);
  parser.process(app);

  // Latch the launch-time debug gate before any UI is built (PreferencesDialog
  // reads it to decide whether to show the chrome-metric scrubbers).
  PJ::setDebugMode(parser.isSet(debug_mode_option));

  // Session-only OpenGL override: applied before any plot is constructed so the
  // first plot already honours it. Leaves Preferences::use_opengl untouched.
  PJ::PlotWidgetBase::setOpenGlDisabledOverride(parser.isSet(disable_opengl_option));

  PJ::MainWindow window(parser.value(plugin_dir_option));

  // App-wide gesture watcher. Observes key presses without consuming them and
  // calls the entry point when the fixed sequence completes.
  auto* gesture_watcher =
      new PJ::KeySequenceWatcher(PJ::unlockSteps(), [&window]() { window.openEmbeddedConsole(); }, &app);
  qApp->installEventFilter(gesture_watcher);

  // Arm autoplay BEFORE any data loads, so its one-shot listener catches the first
  // range — whether --test-data sets it synchronously below or --layout's async
  // load sets it once the worker finishes.
  if (parser.isSet(autoplay_option)) {
    window.enableAutoplay();
  }

  if (parser.isSet(test_data_option)) {
    if (!window.populateTestData()) {
      return EXIT_FAILURE;
    }
  }
  window.show();

  // Deferred so the load runs after the event loop starts (the file loads on a
  // worker; the progressive layout restore needs a running loop).
  if (parser.isSet(layout_option)) {
    const QString layout_path = parser.value(layout_option);
    QTimer::singleShot(0, &window, [&window, layout_path]() { window.loadLayoutAtStartup(layout_path); });
  }

  return app.exec();
}
