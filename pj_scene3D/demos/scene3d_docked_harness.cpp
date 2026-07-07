// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Repro harness for the macOS "3D dock renders blank/stale" bug. It puts a
// SceneViewWidget (a QOpenGLWidget) into the SAME Qt-Advanced-Docking system the
// app uses, beside a normal raster widget, so the top-level window takes Qt's
// full-window RHI composite path (the app scenario a top-level demo can't hit).
//
// It reproduces the app's two dock-creation flows (DockWidget::setObjectWidget /
// the layout-restore path) as closely as possible without the app's services:
//   drop    : window shown first, then the view is setWidget() into the dock
//             (reparent + GL-context recreation while ON screen).
//   restore : the CDockManager is HIDDEN, the view is setWidget() into the dock
//             (GL context realized while the top-level is hidden), THEN shown.
//
// The diagnostic is the SceneViewWidget lifecycle trace (qCInfo
// "pj.scene3d.scene_view": initializeGL / paintGL[first] / showEvent /
// releaseGlResources). "initializeGL ran but no paintGL[first]" after the dock
// swap == paintGL never ran on that context (the blank-texture root cause).
//
// Usage: scene3d_docked_harness [drop|restore]   (default: drop)
// Auto-quits after a few seconds so it is scriptable; run on a REAL window
// platform (cocoa), NOT QT_QPA_PLATFORM=offscreen — the bug is in on-screen
// compositing.

#include <DockManager.h>
#include <DockWidget.h>

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QString>
#include <QSurfaceFormat>
#include <QTimer>

#include "pj_scene3d_widgets/scene_view_widget.h"

int main(int argc, char** argv) {
#if defined(Q_OS_MACOS)
  if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI_BACKEND")) {
    qputenv("QT_WIDGETS_RHI_BACKEND", "opengl");
  }
#endif
  QSurfaceFormat fmt;
#if defined(Q_OS_MACOS)
  fmt.setVersion(4, 1);
#else
  fmt.setVersion(4, 5);
#endif
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  // Experiment toggle: PJ_SHARE_CONTEXTS=1 sets AA_ShareOpenGLContexts (must be
  // before QApplication) to test whether a shared global context lets the widget's
  // GL resources/texture survive the ADS reparent instead of being recreated.
  if (qEnvironmentVariableIntValue("PJ_SHARE_CONTEXTS") != 0) {
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    qInfo("PJ_SHARE_CONTEXTS=1 -> AA_ShareOpenGLContexts set");
  }

  QApplication app(argc, argv);
  const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("drop");
  qInfo("=== scene3d_docked_harness mode=%s ===", qPrintable(mode));

  auto* win = new QMainWindow;
  win->resize(1200, 800);
  auto* dm = new ads::CDockManager(win);  // installs itself as the QMainWindow central widget

  // A normal raster dock so the 3D widget is NOT the window's sole content — this
  // is what forces the full-window RHI composite the app hits (and a top-level
  // demo does not).
  auto* side = new QLabel(QStringLiteral("side panel (raster)"));
  side->setMinimumWidth(220);
  auto* sideDock = new ads::CDockWidget(QStringLiteral("Side"));
  sideDock->setWidget(side);
  dm->addDockWidget(ads::LeftDockWidgetArea, sideDock);

  // The 3D dock: empty CDockWidget added to the center area (this triggers ADS'
  // setParent(nullptr) -> area-layout -> setParent(DockArea) reparent churn).
  auto* viewDock = new ads::CDockWidget(QStringLiteral("Scene3D"));
  dm->addDockWidget(ads::CenterDockWidgetArea, viewDock);

  auto* view = new pj::scene3d::SceneViewWidget;

  auto swapInView = [viewDock, view]() {
    // Mirror DockWidget::setObjectWidget: ForceNoScrollArea, reparenting the
    // QOpenGLWidget into the dock's own layout (GL context (re)created here).
    viewDock->setWidget(view, ads::CDockWidget::ForceNoScrollArea);
  };

  // Force an ADS-style reparent of an ALREADY-realized view: takeWidget() detaches
  // it to a parentless top-level (destroying its GL context) and setWidget()
  // re-attaches it (creating a fresh context) — the setParent churn ADS does when
  // a dock subtree is inserted/moved. This is what recreates the GL context in the
  // app; a plain first-realize does not.
  auto reparentView = [viewDock, view]() {
    viewDock->takeWidget();
    viewDock->setWidget(view, ads::CDockWidget::ForceNoScrollArea);
  };

  if (mode == QStringLiteral("restore")) {
    // Layout-restore path: realize the GL context while the top-level is HIDDEN,
    // then show — the case the single deferred showEvent update() can miss.
    dm->hide();
    win->hide();
    swapInView();
    QTimer::singleShot(300, win, [win, dm]() {
      win->show();
      dm->show();
    });
    QTimer::singleShot(1200, view, reparentView);  // recreate context after show
  } else {
    // Drop path: window on screen first, reparent the view into the dock (realize
    // context A), then force a context recreation like an ADS dock move.
    win->show();
    QTimer::singleShot(500, view, swapInView);
    QTimer::singleShot(1500, view, reparentView);
  }

  // PJ_HARNESS_REPAINT=1 drives a ~60 Hz continuous repaint so PJ_PERF_TRACE has a
  // steady paint stream to aggregate (a static scene otherwise paints on demand).
  if (qEnvironmentVariableIntValue("PJ_HARNESS_REPAINT") != 0) {
    auto* repaint = new QTimer(view);
    QObject::connect(repaint, &QTimer::timeout, view, [view]() { view->update(); });
    repaint->start(16);
  }

  // Auto-quit after PJ_HARNESS_QUIT_MS (default: stay open for visual inspection).
  // Scripted runs set e.g. PJ_HARNESS_QUIT_MS=4000 to capture the stderr trace and
  // exit. The reparent/context-recreation happens ~1.5s in, so leave it running to
  // SEE the post-reparent composite (correct scene vs stale/black).
  const int quit_ms = qEnvironmentVariableIntValue("PJ_HARNESS_QUIT_MS");
  if (quit_ms > 0) {
    QTimer::singleShot(quit_ms, &app, &QApplication::quit);
  }
  return app.exec();
}
