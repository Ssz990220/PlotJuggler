#include "pj_plotting/TabbedPlotWidget.h"

#include <DockManager.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QUuid>
#include <QVector>
#include <algorithm>
#include <utility>

#include "pj_plotting/PlotDocker.h"
#include "pj_widgets/SvgUtil.h"

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
  ads::CDockManager::setConfigFlag(ads::CDockManager::DragPreviewIsDynamic, true);
  done = true;
}

QString newStateId() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
}  // namespace

TabbedPlotWidget::TabbedPlotWidget(QWidget* parent) : TabbedPlotWidget(QStringLiteral("main"), parent) {}

TabbedPlotWidget::TabbedPlotWidget(QString name, QWidget* parent)
    : QWidget(parent), state_id_(newStateId()), name_(std::move(name)) {
  applyAdsConfigOnce();
  setContentsMargins(4, 0, 0, 0);

  auto* main_layout = new QHBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);

  tab_widget_ = new QTabWidget(this);
  tab_widget_->setTabsClosable(true);
  tab_widget_->setMovable(true);

  connect(tab_widget_->tabBar(), &QTabBar::tabBarDoubleClicked, this, &TabbedPlotWidget::onRenameCurrentTab);

  main_layout->addWidget(tab_widget_);

  connect(tab_widget_, &QTabWidget::currentChanged, this, &TabbedPlotWidget::onTabWidgetCurrentChanged);
  connect(tab_widget_, &QTabWidget::tabCloseRequested, this, &TabbedPlotWidget::onTabCloseRequested);
  connect(tab_widget_->tabBar(), &QTabBar::tabMoved, this, [this]() {
    if (!restoring_state_) {
      emit undoableChange();
    }
  });

  tab_widget_->tabBar()->installEventFilter(this);

  addTab({});

  button_add_tab_ = new QPushButton("", this);
  button_add_tab_->setFlat(true);
  button_add_tab_->setFixedSize(QSize(32, 32));
  button_add_tab_->setFocusPolicy(Qt::NoFocus);

  onStylesheetChanged(currentTheme());

  connect(button_add_tab_, &QPushButton::pressed, this, &TabbedPlotWidget::onAddTabButtonPressed);
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

  auto* docker = new PlotDocker(tab_name, session_, catalog_, this);
  connect(docker, &PlotDocker::undoableChange, this, &TabbedPlotWidget::undoableChange);

  tab_widget_->addTab(docker, tab_name);
  emit tabAdded(docker);

  installCloseButton(docker);
  tab_widget_->setCurrentWidget(docker);
  return docker;
}

void TabbedPlotWidget::installCloseButton(PlotDocker* docker) {
  const int index = tab_widget_->indexOf(docker);
  if (index < 0) {
    return;
  }
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
}

void TabbedPlotWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (int index = 0; index < tab_widget_->count(); ++index) {
    if (auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index))) {
      docker->setDataServices(session_, catalog_);
    }
  }
}

void TabbedPlotWidget::onRenameCurrentTab() {
  const int idx = tab_widget_->tabBar()->currentIndex();
  bool ok = true;
  const QString new_name = QInputDialog::getText(
      this, tr("Change the tab name"), tr("New name:"), QLineEdit::Normal, tab_widget_->tabText(idx), &ok);
  if (ok) {
    tab_widget_->setTabText(idx, new_name);
    if (auto* tab = currentTab()) {
      tab->setName(new_name);
    }
    emit undoableChange();
  }
}

void TabbedPlotWidget::onAddTabButtonPressed() {
  addTab({});
  emit undoableChange();
}

void TabbedPlotWidget::onTabWidgetCurrentChanged(int index) {
  if (tab_widget_->count() == 0) {
    if (restoring_state_) {
      return;
    }
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
  if (!restoring_state_) {
    emit undoableChange();
  }
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
  const QIcon close_icon = LoadSvg(":/resources/svg/close-button.svg", theme);
  for (int index = 0; index < tab_widget_->count(); ++index) {
    if (auto* tab_button = tab_widget_->tabBar()->tabButton(index, QTabBar::RightSide)) {
      if (auto* close_button = tab_button->findChild<QPushButton*>()) {
        close_button->setIcon(close_icon);
      }
    }
    if (auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index))) {
      docker->onStylesheetChanged(theme);
    }
  }
}

QString TabbedPlotWidget::stateId() const {
  return state_id_;
}

void TabbedPlotWidget::setStateId(QString id) {
  if (!id.isEmpty()) {
    state_id_ = std::move(id);
  }
}

QDomElement TabbedPlotWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement tabbed_area = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed_area.setAttribute(QStringLiteral("id"), state_id_);
  tabbed_area.setAttribute(QStringLiteral("name"), name_);
  tabbed_area.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));

  for (int index = 0; index < tab_widget_->count(); ++index) {
    auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index));
    if (docker == nullptr) {
      continue;
    }
    QDomElement tab_element = docker->xmlSaveState(doc);
    tab_element.setAttribute(QStringLiteral("tab_name"), tab_widget_->tabText(index));
    tabbed_area.appendChild(tab_element);
  }

  QDomElement current_tab = doc.createElement(QStringLiteral("currentTabIndex"));
  current_tab.setAttribute(QStringLiteral("index"), tab_widget_->currentIndex());
  tabbed_area.appendChild(current_tab);
  return tabbed_area;
}

bool TabbedPlotWidget::xmlLoadState(const QDomElement& tabbed_area) {
  if (tabbed_area.isNull() || tabbed_area.tagName() != QStringLiteral("tabbed_widget")) {
    return false;
  }

  setStateId(tabbed_area.attribute(QStringLiteral("id")));
  if (tabbed_area.hasAttribute(QStringLiteral("name"))) {
    name_ = tabbed_area.attribute(QStringLiteral("name"));
  }

  QVector<QDomElement> target_tabs;
  for (QDomElement tab = tabbed_area.firstChildElement(QStringLiteral("Tab")); !tab.isNull();
       tab = tab.nextSiblingElement(QStringLiteral("Tab"))) {
    target_tabs.push_back(tab);
  }
  if (target_tabs.isEmpty()) {
    return false;
  }

  restoring_state_ = true;

  QVector<PlotDocker*> existing_tabs;
  for (int index = 0; index < tab_widget_->count(); ++index) {
    if (auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index))) {
      existing_tabs.push_back(docker);
    }
  }

  QVector<PlotDocker*> used_tabs;
  auto is_used = [&used_tabs](PlotDocker* candidate) { return used_tabs.contains(candidate); };
  auto find_by_id = [&existing_tabs, &is_used](const QString& id) -> PlotDocker* {
    if (id.isEmpty()) {
      return nullptr;
    }
    for (PlotDocker* docker : existing_tabs) {
      if (docker != nullptr && !is_used(docker) && docker->stateId() == id) {
        return docker;
      }
    }
    return nullptr;
  };
  auto find_by_name = [&existing_tabs, &is_used](const QString& name) -> PlotDocker* {
    if (name.isEmpty()) {
      return nullptr;
    }
    for (PlotDocker* docker : existing_tabs) {
      if (docker != nullptr && !is_used(docker) && docker->name() == name) {
        return docker;
      }
    }
    return nullptr;
  };

  for (qsizetype target_index = 0; target_index < target_tabs.size(); ++target_index) {
    const QDomElement tab_element = target_tabs.at(target_index);
    const QString tab_name =
        tab_element.attribute(QStringLiteral("tab_name"), QStringLiteral("tab%1").arg(target_index + 1));

    PlotDocker* docker = find_by_id(tab_element.attribute(QStringLiteral("id")));
    if (docker == nullptr) {
      docker = find_by_name(tab_name);
    }
    if (docker == nullptr && target_index < existing_tabs.size() && !is_used(existing_tabs.at(target_index))) {
      docker = existing_tabs.at(target_index);
    }
    if (docker == nullptr) {
      docker = addTab(tab_name);
    }

    used_tabs.push_back(docker);
    docker->setStateId(tab_element.attribute(QStringLiteral("id")));
    docker->setName(tab_name);

    const int current_index = tab_widget_->indexOf(docker);
    if (current_index != target_index) {
      tab_widget_->removeTab(current_index);
      tab_widget_->insertTab(static_cast<int>(target_index), docker, tab_name);
      installCloseButton(docker);
    } else {
      tab_widget_->setTabText(current_index, tab_name);
    }

    if (!docker->xmlLoadState(tab_element)) {
      restoring_state_ = false;
      return false;
    }
  }

  for (int index = tab_widget_->count() - 1; index >= 0; --index) {
    auto* docker = qobject_cast<PlotDocker*>(tab_widget_->widget(index));
    if (docker != nullptr && !used_tabs.contains(docker)) {
      tab_widget_->removeTab(index);
      docker->deleteLater();
    }
  }

  int current_index = tabbed_area.firstChildElement(QStringLiteral("currentTabIndex"))
                          .attribute(QStringLiteral("index"), QStringLiteral("0"))
                          .toInt();
  current_index = std::clamp(current_index, 0, tab_widget_->count() - 1);
  tab_widget_->setCurrentIndex(current_index);
  onTabWidgetCurrentChanged(current_index);
  restoring_state_ = false;
  return true;
}

}  // namespace PJ
