#include "pj_plot_widgets/TabbedPlotWidget.h"

#include <DockManager.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <algorithm>

#include "pj_app_core/SvgUtil.h"
#include "pj_plot_widgets/PlotDocker.h"

namespace PJ {

namespace {
// ADS config flags are process-global and must be set before the first
// CDockManager is instantiated. Call once, lazily.
void applyAdsConfigOnce() {
  static bool done = false;
  if (done) {
    return;
  }
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasTabsMenuButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasUndockButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::DockAreaHasCloseButton, false);
  ads::CDockManager::setConfigFlag(ads::CDockManager::EqualSplitOnInsertion, true);
  ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
  done = true;
}
}  // namespace

TabbedPlotWidget::TabbedPlotWidget(QWidget* parent) : TabbedPlotWidget(QStringLiteral("main"), parent) {}

TabbedPlotWidget::TabbedPlotWidget(QString name, QWidget* parent)
    : QWidget(parent), name_(std::move(name)) {
  applyAdsConfigOnce();
  setContentsMargins(0, 0, 0, 0);

  auto* main_layout = new QHBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);

  tab_widget_ = new QTabWidget(this);
  tab_widget_->setTabsClosable(true);
  tab_widget_->setMovable(true);

  connect(tab_widget_->tabBar(), &QTabBar::tabBarDoubleClicked, this,
          &TabbedPlotWidget::onRenameCurrentTab);

  main_layout->addWidget(tab_widget_);

  connect(tab_widget_, &QTabWidget::currentChanged, this,
          &TabbedPlotWidget::onTabWidgetCurrentChanged);
  connect(tab_widget_, &QTabWidget::tabCloseRequested, this,
          &TabbedPlotWidget::onTabCloseRequested);

  tab_widget_->tabBar()->installEventFilter(this);

  addTab({});

  button_add_tab_ = new QPushButton("", this);
  button_add_tab_->setFlat(true);
  button_add_tab_->setFixedSize(QSize(32, 32));
  button_add_tab_->setFocusPolicy(Qt::NoFocus);

  onStylesheetChanged(currentTheme());

  connect(button_add_tab_, &QPushButton::pressed, this,
          &TabbedPlotWidget::onAddTabButtonPressed);
}

TabbedPlotWidget::~TabbedPlotWidget() = default;

void TabbedPlotWidget::paintEvent(QPaintEvent* event) {
  QWidget::paintEvent(event);
  const int margin = 5;
  const int width_tabbar = tab_widget_->tabBar()->width();
  const int max_width_tabbar = std::max(0, width() - button_add_tab_->width() - margin);
  const int button_pos = std::min(width_tabbar + margin, max_width_tabbar);
  tab_widget_->tabBar()->setMaximumWidth(max_width_tabbar);
  button_add_tab_->move(QPoint(button_pos, 0));
}

PlotDocker* TabbedPlotWidget::currentTab() {
  return qobject_cast<PlotDocker*>(tab_widget_->currentWidget());
}

PlotDocker* TabbedPlotWidget::addTab(QString tab_name) {
  if (tab_name.isEmpty()) {
    tab_name = QString("tab%1").arg(++tab_suffix_count_);
  }

  auto* docker = new PlotDocker(tab_name, this);
  connect(docker, &PlotDocker::undoableChange, this, &TabbedPlotWidget::undoableChange);

  tab_widget_->addTab(docker, tab_name);
  emit tabAdded(docker);

  const int index = tab_widget_->count() - 1;

  auto* button_widget = new QWidget();
  auto* layout = new QHBoxLayout(button_widget);
  layout->setSpacing(2);
  layout->setContentsMargins(0, 0, 0, 0);

  auto* close_button = new QPushButton();
  close_button->setIcon(LoadSvg(":/resources/svg/close-button.svg", currentTheme()));
  close_button->setFixedSize(QSize(16, 16));
  close_button->setFlat(true);
  // Capture the dock pointer rather than an index — indexOf resolves the
  // correct tab at click time even if the tab order has changed.
  connect(close_button, &QPushButton::pressed, this, [this, docker]() {
    const int idx = tab_widget_->indexOf(docker);
    if (idx >= 0) {
      onTabCloseRequested(idx);
    }
  });

  layout->addWidget(close_button);
  tab_widget_->tabBar()->setTabButton(index, QTabBar::RightSide, button_widget);

  tab_widget_->setCurrentWidget(docker);
  return docker;
}

void TabbedPlotWidget::onRenameCurrentTab() {
  const int idx = tab_widget_->tabBar()->currentIndex();
  bool ok = true;
  const QString new_name = QInputDialog::getText(this, tr("Change the tab name"), tr("New name:"),
                                                 QLineEdit::Normal, tab_widget_->tabText(idx), &ok);
  if (ok) {
    tab_widget_->setTabText(idx, new_name);
    if (auto* tab = currentTab()) {
      tab->setName(new_name);
    }
  }
}

void TabbedPlotWidget::onAddTabButtonPressed() {
  addTab({});
  emit undoableChange();
}

void TabbedPlotWidget::onTabWidgetCurrentChanged(int index) {
  if (tab_widget_->count() == 0) {
    addTab({});
  }
  for (int i = 0; i < tab_widget_->count(); ++i) {
    auto* button = tab_widget_->tabBar()->tabButton(i, QTabBar::RightSide);
    if (button) {
      button->setHidden(i != index);
    }
  }
}

void TabbedPlotWidget::onTabCloseRequested(int index) {
  // Always keep at least one tab open.
  if (tab_widget_->count() == 1) {
    onAddTabButtonPressed();
  }

  auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index));
  tab_widget_->removeTab(index);
  if (docker) {
    docker->deleteLater();
  }
  emit undoableChange();
}

bool TabbedPlotWidget::eventFilter(QObject* obj, QEvent* event) {
  QTabBar* tab_bar = tab_widget_->tabBar();
  if (obj == tab_bar && event->type() == QEvent::MouseButtonPress) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    const int index = tab_bar->tabAt(mouse_event->pos());
    tab_bar->setCurrentIndex(index);
  }
  return QWidget::eventFilter(obj, event);
}

void TabbedPlotWidget::onStylesheetChanged(QString theme) {
  if (button_add_tab_) {
    button_add_tab_->setIcon(LoadSvg(":/resources/svg/add_tab.svg", theme));
  }
}

}  // namespace PJ
