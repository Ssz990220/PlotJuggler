// Standalone reproducer for the ADS drag-to-rearrange behavior in PJ4.
//
// Mirrors PJ4's setup exactly:
//   - HiddenTitleBar (always invisible, real CDockAreaTitleBar instance)
//   - SplittableComponentsFactory installs HiddenTitleBar
//   - Custom MockToolbar inserted as the first widget in each dock's layout
//   - MockToolbar forwards mousePress/Move/Release to the hidden title bar
//     via QCoreApplication::sendEvent
//   - DockWidgetFloatable=false, DockWidgetDeleteOnClose=true
//   - DockWidgetMovable left at the ADS default (true)
//
// All extras (PlotWidget/PlotWidgetBase, Qwt, event filters on canvases) are
// removed so we can verify the puppet-forwarding chain in isolation.
//
// std::cerr traces are emitted at every event entry point and at every ADS
// drag-state transition we can reach from the embedder side.

#include <DockAreaTitleBar.h>
#include <DockAreaWidget.h>
#include <DockComponentsFactory.h>
#include <DockManager.h>
#include <DockWidget.h>
#include <FloatingDragPreview.h>

#include <QApplication>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdlib>
#include <iostream>

namespace {

// Same trick PJ3/PJ4 use: keep the title bar alive (so area->titleBar()
// returns a real CDockAreaTitleBar with its drag state machine) but never
// let it become visible.
class HiddenTitleBar : public ads::CDockAreaTitleBar {
 public:
  using ads::CDockAreaTitleBar::CDockAreaTitleBar;
  void setVisible(bool /*visible*/) override {
    QWidget::setVisible(false);
  }
};

class SplittableComponentsFactory : public ads::CDockComponentsFactory {
 public:
  ads::CDockAreaTitleBar* createDockAreaTitleBar(ads::CDockAreaWidget* dock_area) const override {
    auto* title_bar = new HiddenTitleBar(dock_area);
    title_bar->setVisible(false);
    std::cerr << "[factory] created HiddenTitleBar=" << title_bar << " for area=" << dock_area << "\n";
    return title_bar;
  }
};

// Minimal stand-in for PJ4's DockToolbar. Visible 28-px strip; mouse events
// forwarded to the (invisible) ADS title bar.
class MockToolbar : public QWidget {
 public:
  explicit MockToolbar(ads::CDockWidget* parent, const QString& label) : QWidget(parent), parent_dock_(parent) {
    setFixedHeight(28);
    setStyleSheet("background-color: #4080c0;");
    setMouseTracking(true);
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(8, 0, 8, 0);
    auto* lbl = new QLabel(label, this);
    lbl->setStyleSheet("color: white; font-weight: bold;");
    l->addWidget(lbl);
    l->addStretch();
    label_text_ = label.toStdString();
  }

 private:
  void mousePressEvent(QMouseEvent* ev) override {
    std::cerr << "[" << label_text_ << "] mousePressEvent button=" << int(ev->button()) << " pos=(" << ev->pos().x()
              << "," << ev->pos().y() << ") accepted-on-entry=" << ev->isAccepted() << "\n";
    if (auto* area = parent_dock_->dockAreaWidget()) {
      auto* title_bar = area->titleBar();
      // Forward with pos=(0,0) so CFloatingDragPreview positions its top-left
      // at the cursor (no horizontal offset based on where in the wide toolbar
      // the user happened to click).
      QMouseEvent fwd(
          QEvent::MouseButtonPress, QPointF(0, 0), ev->globalPosition(), ev->button(), ev->buttons(), ev->modifiers());
      std::cerr << "  -> forwarding (synthetic pos=(0,0)) to titleBar=" << title_bar
                << " visible=" << title_bar->isVisible() << "\n";
      QCoreApplication::sendEvent(title_bar, &fwd);
      ev->setAccepted(fwd.isAccepted());
      std::cerr << "  <- after sendEvent: accepted=" << ev->isAccepted() << "\n";
    } else {
      std::cerr << "  !! no dockAreaWidget\n";
    }
  }

  void mouseReleaseEvent(QMouseEvent* ev) override {
    std::cerr << "[" << label_text_ << "] mouseReleaseEvent button=" << int(ev->button()) << " pos=(" << ev->pos().x()
              << "," << ev->pos().y() << ") globalPos=(" << ev->globalPosition().x() << "," << ev->globalPosition().y()
              << ")\n";
    if (auto* area = parent_dock_->dockAreaWidget()) {
      auto* title_bar = area->titleBar();
      QCoreApplication::sendEvent(title_bar, ev);
      std::cerr << "  <- release accepted=" << ev->isAccepted() << "\n";
    } else {
      std::cerr << "  !! no dockAreaWidget on release\n";
    }
  }

  void mouseMoveEvent(QMouseEvent* ev) override {
    static int counter = 0;
    const int n = counter++;
    // Throttle the chatty "we forwarded" trace; emit the Wayland-validation
    // sample on the same cadence so the two streams stay aligned.
    if ((n % 10) == 0) {
      const QPoint cursor_pos = QCursor::pos();
      const QPoint event_pos = ev->globalPosition().toPoint();
      const QPoint delta = event_pos - cursor_pos;

      std::cerr << "[" << label_text_ << "] mv#" << n << " platform=" << QGuiApplication::platformName().toStdString()
                << " QCursor::pos=(" << cursor_pos.x() << "," << cursor_pos.y() << ")"
                << " ev.globalPos=(" << event_pos.x() << "," << event_pos.y() << ")"
                << " delta=(" << delta.x() << "," << delta.y() << ")";

      // Find the live drag preview, if any. ADS creates a CFloatingDragPreview
      // top-level once startFloating() fires; finding it via qApp->topLevelWidgets
      // works whether it is parented to the manager or not.
      ads::CFloatingDragPreview* preview = nullptr;
      for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* p = qobject_cast<ads::CFloatingDragPreview*>(w)) {
          preview = p;
          break;
        }
      }
      if (preview) {
        const QPoint frame_tl = preview->frameGeometry().topLeft();
        const QPoint cursor_to_frame = cursor_pos - frame_tl;
        std::cerr << " preview.frame.tl=(" << frame_tl.x() << "," << frame_tl.y() << ")"
                  << " cursor->frame=(" << cursor_to_frame.x() << "," << cursor_to_frame.y() << ")"
                  << " preview.visible=" << preview->isVisible();
      } else {
        std::cerr << " preview=<none>";
      }
      std::cerr << "\n";
    }
    if (auto* area = parent_dock_->dockAreaWidget()) {
      QCoreApplication::sendEvent(area->titleBar(), ev);
    }
    ev->accept();
    QWidget::mouseMoveEvent(ev);
  }

  ads::CDockWidget* parent_dock_;
  std::string label_text_;
};

ads::CDockWidget* makeDock(ads::CDockManager* manager, const QString& title) {
  auto* dock = new ads::CDockWidget(manager, title);
  // Match PJ4 features (after the puppet-forwarding fix).
  dock->setFeature(ads::CDockWidget::DockWidgetFloatable, false);
  dock->setFeature(ads::CDockWidget::DockWidgetDeleteOnClose, true);

  auto* body = new QLabel(QStringLiteral("Body of %1\n\n(grab the blue strip above)").arg(title), dock);
  body->setAlignment(Qt::AlignCenter);
  body->setStyleSheet("background-color: white; padding: 24px;");
  dock->setWidget(body);

  // Same insertion pattern as PJ4 DockWidget::DockWidget — toolbar inserted as
  // the first child of the dock's existing QBoxLayout, *above* the content.
  auto* toolbar = new MockToolbar(dock, title);
  qobject_cast<QBoxLayout*>(dock->layout())->insertWidget(0, toolbar);

  return dock;
}

}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  // Match PJ4 TabbedPlotWidget::applyAdsConfigOnce.
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::EqualSplitOnInsertion, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DragPreviewIsDynamic, true);

  QMainWindow window;
  window.resize(960, 640);
  window.setWindowTitle(QStringLiteral("ADS Dock Drag Repro"));

  auto* manager = new ads::CDockManager(&window);
  manager->setStyleSheet({});  // disable ADS built-in stylesheet, like PJ4 PlotDocker
  manager->setComponentsFactory(new SplittableComponentsFactory());
  window.setCentralWidget(manager);

  auto* a = makeDock(manager, "A");
  auto* b = makeDock(manager, "B");
  auto* c = makeDock(manager, "C");
  auto* d = makeDock(manager, "D");

  auto* area_a = manager->addDockWidget(ads::CenterDockWidgetArea, a);
  area_a->setAllowedAreas(ads::OuterDockAreas);
  auto* area_b = manager->addDockWidget(ads::RightDockWidgetArea, b, area_a);
  area_b->setAllowedAreas(ads::OuterDockAreas);
  auto* area_c = manager->addDockWidget(ads::BottomDockWidgetArea, c, area_a);
  area_c->setAllowedAreas(ads::OuterDockAreas);
  auto* area_d = manager->addDockWidget(ads::BottomDockWidgetArea, d, area_b);
  area_d->setAllowedAreas(ads::OuterDockAreas);

  std::cerr << "[main] Ready. Drag a blue strip to rearrange. Cancel with Esc.\n";
  std::cerr << "[main] platform=" << QGuiApplication::platformName().toStdString() << "\n";
  window.show();

  // Auto-drag mode for headless / gdb-driven crash reproduction. Set
  // PJ_AUTO_DRAG=1 in the environment to fire a synthetic press → moves →
  // release sequence on dock A's toolbar 600ms after show(), simulating a
  // drag from A's strip toward the bottom-right corner.
  if (const char* autodrag = std::getenv("PJ_AUTO_DRAG"); autodrag && std::string_view(autodrag) == "1") {
    QPointer<ads::CDockWidget> srcDock = a;
    QTimer::singleShot(600, [srcDock]() {
      if (!srcDock) {
        return;
      }
      auto* toolbar = qobject_cast<QWidget*>(srcDock->layout()->itemAt(0)->widget());
      if (!toolbar) {
        std::cerr << "[autodrag] could not find toolbar\n";
        return;
      }
      const QPoint local_press(40, 14);
      const QPoint global_press = toolbar->mapToGlobal(local_press);
      std::cerr << "[autodrag] press at global=(" << global_press.x() << "," << global_press.y() << ")\n";

      QMouseEvent press(
          QEvent::MouseButtonPress, QPointF(local_press), QPointF(global_press), Qt::LeftButton, Qt::LeftButton,
          Qt::NoModifier);
      QCoreApplication::sendEvent(toolbar, &press);

      // Move in 8 steps toward (toolbar global + (300, 250))
      const QPoint global_target = global_press + QPoint(300, 250);
      for (int i = 1; i <= 8; ++i) {
        const QPoint step_global = global_press + (global_target - global_press) * i / 8;
        const QPoint step_local = toolbar->mapFromGlobal(step_global);
        QMouseEvent move(
            QEvent::MouseMove, QPointF(step_local), QPointF(step_global), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        std::cerr << "[autodrag] move step " << i << " global=(" << step_global.x() << "," << step_global.y() << ")\n";
        QCoreApplication::sendEvent(toolbar, &move);
      }

      QMouseEvent release(
          QEvent::MouseButtonRelease, QPointF(toolbar->mapFromGlobal(global_target)), QPointF(global_target),
          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
      std::cerr << "[autodrag] release at global=(" << global_target.x() << "," << global_target.y() << ")\n";
      QCoreApplication::sendEvent(toolbar, &release);

      std::cerr << "[autodrag] sequence complete; quitting in 200ms\n";
      QTimer::singleShot(200, []() { QCoreApplication::quit(); });
    });
  }

  return app.exec();
}
