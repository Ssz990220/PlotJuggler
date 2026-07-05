// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QGuiApplication>
#include <QImage>
#include <QLoggingCategory>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QSplashScreen>
#include <QThread>
#include <QTimer>
#include <Qt>
#include <backward.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "DebugMode.h"
#include "KeySequence.h"
#include "MainWindow.h"
#include "Splashscreen.h"
#include "WidgetTuner.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_scene3d_widgets/scene_view_widget.h"  // --screenshot grabs the 3D view
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
  // PJ_SCENE3D_TRACE=1: verbose lifecycle trace for the macOS docked-3D composite
  // investigation. Timestamp + category on every line; the categories themselves
  // are enabled below (after QApplication). Must set the message pattern before Qt
  // installs its default handler. No-op (and near-zero cost) when unset.
  const bool scene3d_trace = qEnvironmentVariableIntValue("PJ_SCENE3D_TRACE") != 0;
  if (scene3d_trace) {
    qputenv("QT_MESSAGE_PATTERN", "%{time process}s [%{category}] %{message}");
  }

#if defined(Q_OS_MACOS)
  // Force Qt's widget compositor to its OpenGL backend on macOS. As soon as a
  // QOpenGLWidget (the 3D SceneViewWidget) exists, Qt 6 composites the WHOLE
  // top-level window through QRhi. The default backend on macOS is Metal, whose
  // texture origin is top-left while the widget's GL content is bottom-left, so
  // the Metal compositor blits the composited window upside down (the entire app
  // renders as a vertical mirror the moment a 3D dock is added). The OpenGL RHI
  // backend shares the GL orientation, so the window composites right-side-up.
  // Must be set BEFORE QApplication constructs the platform/RHI integration.
  // Respect an explicit user override.
  if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI_BACKEND")) {
    qputenv("QT_WIDGETS_RHI_BACKEND", "opengl");
  }
#endif

  // Pin to Fusion (under our Style proxy) before constructing
  // QApplication so widgets that read the style at construction time
  // don't end up with the platform's native style (KDE Breeze, GNOME
  // Adwaita, etc.) which silently overrides QSS on QMenu and other
  // popups. Style additionally suppresses default dialog-button icons
  // and the underline-mnemonic decoration.
  QApplication::setStyle(new PJ::Style(QStringLiteral("Fusion")));

  // Qt::AA_ShareOpenGLContexts — historically DISABLED, now an experimental opt-in
  // (PJ_SHARE_CONTEXTS=1), default OFF.
  //
  // Why it was disabled: it put every SceneViewWidget's context into one share
  // group, and destroying one view's context (closing/splitting a 3D dock)
  // corrupted the VAO/FBO state of sibling views still on screen — a
  // glBindVertexArray(non-gen name) flood + the map texture vanishing in the
  // surviving view. The workaround was to keep every view's context fully
  // independent so tearing one down could not touch the others.
  //
  // Why we want it back on macOS: with independent contexts, an ADS dock reparent
  // DESTROYS+RECREATES the QOpenGLWidget's context, and Qt's macOS RHI compositor
  // does not re-acquire the recreated context's texture — the docked 3D (and the
  // Api::OpenGL MediaViewer) composite a stale/black frame even though paintGL
  // renders correctly into the widget FBO (verified by scene3d_docked_harness +
  // the SceneViewWidget paint trace). AA_ShareOpenGLContexts keeps the context
  // ALIVE across the reparent (the harness shows a single initializeGL, no
  // releaseGL/re-init), which fixes the handoff.
  //
  // Why the old corruption should no longer bite: the gl/ layer now scopes every
  // glDelete* to the OWNING context — gl::Program/Buffer/Texture/etc. capture
  // owning_context_ and only delete when deleteAllowedInCurrentContext() holds, so
  // one view's teardown deletes only ITS names, never a sibling's. VAOs are never
  // shared even with this attribute (container objects), and each view creates +
  // uses its VAOs only in its own context. This must still be revalidated with 2+
  // 3D docks + an image dock (open, then close/split one) before the default flips.
  if (qEnvironmentVariableIntValue("PJ_SHARE_CONTEXTS") != 0) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
  }

  QApplication app(argc, argv);

  if (scene3d_trace) {
    // Turn on the module trace categories (qCDebug, off by default) plus Qt's own
    // RHI / OpenGL / backingstore composition logging, so the whole chain is
    // visible in one stream. Programmatic so the user only needs PJ_SCENE3D_TRACE=1.
    QLoggingCategory::setFilterRules(QStringLiteral(
        "pj.scene3d.scene_view.debug=true\n"
        "pj.scene3d.dock.debug=true\n"
        "pj.app.main.debug=true\n"
        "pj.plotting.dock.debug=true\n"
        "pj.scene2d.media_viewer.debug=true\n"
        "qt.rhi.general=true\n"
        "qt.rhi.backend=true\n"
        "qt.opengl.*=true\n"
        "qt.widgets.painting=true"));
    qInfo().noquote() << "[pjtrace] startup: PJ_SHARE_CONTEXTS gate ->"
                      << "AA_ShareOpenGLContexts=" << QCoreApplication::testAttribute(Qt::AA_ShareOpenGLContexts)
                      << "| QT_WIDGETS_RHI_BACKEND=" << qEnvironmentVariable("QT_WIDGETS_RHI_BACKEND", QStringLiteral("<unset>"))
                      << "| Qt=" << qVersion() << "| platform=" << QGuiApplication::platformName();
  }

  QCoreApplication::setOrganizationName(QStringLiteral("PlotJuggler"));
  QCoreApplication::setApplicationName(QStringLiteral("PlotJuggler4"));
  // PJ_VERSION_STRING comes from the root project(VERSION) via pj_app's
  // target_compile_definitions — the single source of truth read by the About
  // box and compared against the latest GitHub release.
  QCoreApplication::setApplicationVersion(QStringLiteral(PJ_VERSION_STRING));
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
  const QCommandLineOption nosplash_option(
      QStringList() << QStringLiteral("n") << QStringLiteral("nosplash"),
      QStringLiteral("Don't display the splashscreen on startup."));
  parser.addOption(nosplash_option);
  // Dev-only splash preview, disabled but kept for future tweaks: renders the
  // configured splash to a PNG and exits (see the matching handler below).
  // const QCommandLineOption dump_splash_option(
  //     QStringLiteral("dump-splash"),
  //     QStringLiteral("Render the configured startup splashscreen to a PNG and exit (dev preview)."),
  //     QStringLiteral("path"));
  // parser.addOption(dump_splash_option);
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
  // Headless 3D capture for verification: after --screenshot-delay ms (enough for an
  // async --layout load + a couple seconds of --autoplay to pose the robot), grab the
  // first SceneViewWidget's framebuffer to a PNG and quit. GNOME Wayland blocks
  // external screen-capture tools, so the app must grab itself.
  const QCommandLineOption screenshot_option(
      QStringLiteral("screenshot"),
      QStringLiteral("Grab the first 3D view to a PNG after --screenshot-delay, then exit."), QStringLiteral("path"));
  parser.addOption(screenshot_option);
  const QCommandLineOption screenshot_delay_option(
      QStringLiteral("screenshot-delay"), QStringLiteral("ms to wait before the screenshot grab (default 7000)."),
      QStringLiteral("ms"), QStringLiteral("7000"));
  parser.addOption(screenshot_delay_option);
  parser.process(app);

  // Latch the launch-time debug gate before any UI is built (PreferencesDialog
  // reads it to decide whether to show the chrome-metric scrubbers).
  PJ::setDebugMode(parser.isSet(debug_mode_option));

  // Session-only OpenGL override: applied before any plot is constructed so the
  // first plot already honours it. Leaves Preferences::use_opengl untouched.
  PJ::PlotWidgetBase::setOpenGlDisabledOverride(parser.isSet(disable_opengl_option));

  // Dev preview (disabled, kept for future tweaks): render the configured splash
  // to a PNG and exit — lets us inspect the "serious" splash without launching
  // (and without screen-capture, which GNOME Wayland blocks). Re-enable together
  // with the dump_splash_option declaration above.
  // if (parser.isSet(dump_splash_option)) {
  //   const QString path = parser.value(dump_splash_option);
  //   const bool ok = PJ::makeStartupSplash().save(path);
  //   std::fprintf(ok ? stdout : stderr, "[dump-splash] %s: %s\n", ok ? "saved" : "FAILED", qPrintable(path));
  //   return ok ? EXIT_SUCCESS : EXIT_FAILURE;
  // }

  // The funny splashscreen: a random meme that covers the (slow) MainWindow
  // construction below. Skipped with --nosplash and when launching straight
  // into a layout (--layout), where the user wants data, not a meme. Shown
  // before the MainWindow ctor so it's already on screen while the ctor runs.
  std::unique_ptr<QSplashScreen> splash;
  if (!parser.isSet(nosplash_option) && !parser.isSet(layout_option)) {
    const QPixmap pixmap = PJ::makeStartupSplash();
    if (!pixmap.isNull()) {
      splash = std::make_unique<QSplashScreen>(pixmap, Qt::WindowStaysOnTopHint);
      if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        splash->move(screen->availableGeometry().center() - splash->rect().center());
      }
      splash->show();
      app.processEvents();
    }
  }

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
  if (splash) {
    // Keep the meme up briefly so it's actually seen, but let a click dismiss
    // it early: QSplashScreen hides itself on mousePressEvent, so once the user
    // clicks it isHidden() flips and we stop waiting. msleep keeps the spin off
    // the CPU while still pumping events so the click is delivered.
    //
    // The main window is shown only AFTER this loop: a still-hidden main window
    // can't be stacked above the splash, so the meme stays on top. (Wayland
    // ignores WindowStaysOnTopHint / raise() once the main window is up, which
    // is exactly how the splash ended up behind it.)
    const QDateTime deadline = QDateTime::currentDateTime().addMSecs(4000);
    while (QDateTime::currentDateTime() < deadline && !splash->isHidden()) {
      app.processEvents();
      QThread::msleep(20);
    }
  }

  window.show();

  if (splash) {
    // Close the splash once the main window is up.
    splash->finish(&window);
  }

  // Deferred so the load runs after the event loop starts (the file loads on a
  // worker; the progressive layout restore needs a running loop).
  if (parser.isSet(layout_option)) {
    const QString layout_path = parser.value(layout_option);
    QTimer::singleShot(0, &window, [&window, layout_path]() { window.loadLayoutAtStartup(layout_path); });
  }

  // One-shot GitHub release check, opt-out via Preferences (default on) and
  // skipped for headless --screenshot runs. Deferred to the running event loop
  // (QNetworkAccessManager needs it); failures/no-release are silent.
  if (!parser.isSet(screenshot_option) &&
      QSettings().value(QStringLiteral("Preferences::check_updates_on_startup"), true).toBool()) {
    QTimer::singleShot(0, &window, [&window]() { window.checkForUpdates(/*interactive=*/false); });
  }

  if (parser.isSet(screenshot_option)) {
    const QString path = parser.value(screenshot_option);
    const int delay_ms = parser.value(screenshot_delay_option).toInt();
    QTimer::singleShot(delay_ms, &window, [&window, path]() {
      const QList<pj::scene3d::SceneViewWidget*> views = window.findChildren<pj::scene3d::SceneViewWidget*>();
      if (views.isEmpty()) {
        std::fprintf(stderr, "[screenshot] no 3D SceneViewWidget found\n");
      } else {
        // grabFramebuffer() renders paintGL() on demand, so it returns a fresh frame
        // with no separate update()/second timer needed.
        const QImage img = views.first()->grabFramebuffer();
        if (img.save(path)) {
          std::printf("[screenshot] saved: %s (%dx%d)\n", qPrintable(path), img.width(), img.height());
        } else {
          std::fprintf(stderr, "[screenshot] save FAILED: %s\n", qPrintable(path));
        }
      }
      QCoreApplication::quit();
    });
  }

  return app.exec();
}
