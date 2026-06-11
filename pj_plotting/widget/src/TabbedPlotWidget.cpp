// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/TabbedPlotWidget.h"

#include <DockManager.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <utility>

#include "pj_plotting/PlotDocker.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

namespace {
// Compact tab-strip height. We aim for the tab column's *total* chrome
// (the strip itself plus the 1-px tabs_separator drawn underneath) to
// match the Sources/Datasets header bars, which are 24 px and have no
// separator below. So the strip itself is 23 px and the separator
// adds the final 1 px → 24 total — keeping the plot content's top
// edge pixel-aligned with the curve-list content on the left column.
constexpr int kTabBarHeight = 23;
// Tab/button footprint inside the bar — flush with the bar height
// (no breathing room) because tabs have their own coloured backgrounds;
// any gap would show a strip of bar-background above and below the tab.
// The [+] and panel-toggle buttons match so the whole row is uniform.
constexpr int kTabBarButtonSize = 23;
// Icon size — matches the title bar's 20-px glyph so the tab-strip
// buttons read at the same visual weight as the rest of the chrome.
constexpr int kTabBarIconSize = 20;

// QScrollArea variant that redirects vertical wheel input to the
// horizontal scrollbar. Lets the user wheel-scroll an overflowing
// tab strip the same way Chrome / VSCode do, without us needing to
// show a (visually heavy) horizontal scrollbar.
class HorizontalScrollArea : public QScrollArea {
 public:
  using QScrollArea::QScrollArea;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    QScrollBar* hbar = horizontalScrollBar();
    const int delta = event->angleDelta().y();
    if (hbar != nullptr && delta != 0) {
      hbar->setValue(hbar->value() - delta);
      event->accept();
      return;
    }
    QScrollArea::wheelEvent(event);
  }
};

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
  ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
  done = true;
}
}  // namespace

// Small private widget for one tab in the strip. Holds a name label
// plus a close button. Double-clicking flips the label out for an
// in-place QLineEdit; pressing Enter commits the rename, Escape or
// loss of focus reverts to the previous name.
class PlotTabFrame : public QFrame {
  Q_OBJECT
 public:
  explicit PlotTabFrame(const QString& tab_name, QWidget* parent = nullptr) : QFrame(parent) {
    setObjectName("plotTabFrame");
    setProperty("selected", false);
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    // Fill the bar height exactly — tabs have a colored background, so
    // any breathing room above or below would render as a visible
    // 1-px stripe of bar-background flanking the tab.
    setFixedHeight(kTabBarButtonSize);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 0, 4, 0);
    layout->setSpacing(6);

    label_ = new QLabel(tab_name, this);
    label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(label_);

    edit_ = new QLineEdit(this);
    edit_->setObjectName("plotTabRenameEdit");
    edit_->hide();
    edit_->installEventFilter(this);
    connect(edit_, &QLineEdit::returnPressed, this, &PlotTabFrame::commitEdit);
    layout->addWidget(edit_);

    close_button_ = new QPushButton(this);
    close_button_->setFlat(true);
    close_button_->setFixedSize(QSize(16, 16));
    close_button_->setFocusPolicy(Qt::NoFocus);
    layout->addWidget(close_button_);
    connect(close_button_, &QPushButton::clicked, this, &PlotTabFrame::closeRequested);
  }

  void setName(const QString& tab_name) {
    label_->setText(tab_name);
  }

  [[nodiscard]] QString name() const {
    return label_->text();
  }

  void setSelected(bool selected) {
    if (property("selected").toBool() == selected) {
      return;
    }
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
    update();
  }

  [[nodiscard]] QPushButton* closeButton() const {
    return close_button_;
  }

  void enterEditMode() {
    if (edit_active_) {
      return;
    }
    edit_active_ = true;
    edit_->setText(label_->text());
    label_->hide();
    edit_->show();
    edit_->setFocus(Qt::OtherFocusReason);
    edit_->setCursorPosition(edit_->text().length());
  }

 signals:
  void clicked();
  void renameCommitted(const QString& new_name);
  void closeRequested();

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      emit clicked();
    }
    QFrame::mousePressEvent(event);
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton) {
      enterEditMode();
    }
    QFrame::mouseDoubleClickEvent(event);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == edit_ && edit_active_) {
      if (event->type() == QEvent::FocusOut) {
        // Reverting on focus loss is the user-requested behaviour;
        // explicit commit only happens on Enter.
        cancelEdit();
      } else if (event->type() == QEvent::KeyPress) {
        if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
          cancelEdit();
          return true;
        }
      }
    }
    return QFrame::eventFilter(watched, event);
  }

 private slots:
  void commitEdit() {
    if (!edit_active_) {
      return;
    }
    edit_active_ = false;
    const QString new_text = edit_->text();
    label_->setText(new_text);
    edit_->hide();
    label_->show();
    setFocus(Qt::OtherFocusReason);
    emit renameCommitted(new_text);
  }

  void cancelEdit() {
    if (!edit_active_) {
      return;
    }
    edit_active_ = false;
    edit_->hide();
    label_->show();
  }

 private:
  QLabel* label_;
  QLineEdit* edit_;
  QPushButton* close_button_;
  bool edit_active_ = false;
};

TabbedPlotWidget::TabbedPlotWidget(QWidget* parent) : TabbedPlotWidget(QStringLiteral("main"), parent) {}

TabbedPlotWidget::TabbedPlotWidget(QString name, QWidget* parent) : QWidget(parent), name_(std::move(name)) {
  applyAdsConfigOnce();
  setContentsMargins(0, 0, 0, 0);

  auto* root_layout = new QVBoxLayout(this);
  root_layout->setContentsMargins(0, 0, 0, 0);
  root_layout->setSpacing(0);

  // Tab strip — outer hbox with two regions:
  //   * a horizontally scrollable area containing the [+] add button
  //     and one PlotTabFrame per tab (left-aligned via a trailing
  //     stretch). When the tabs together exceed the viewport, the
  //     scroll area shows a horizontal scrollbar instead of squeezing
  //     the panel buttons off-screen.
  //   * the three panel-toggle buttons, pinned at the far right.
  auto* tabs_bar_widget = new QWidget(this);
  tabs_bar_widget->setObjectName("plotTabsBar");
  tabs_bar_widget->setContentsMargins(0, 0, 0, 0);
  auto* outer_layout = new QHBoxLayout(tabs_bar_widget);
  outer_layout->setContentsMargins(0, 0, 0, 0);
  outer_layout->setSpacing(0);

  // QScrollArea-based wrapper removed — the scroll area's viewport was
  // reserving a couple of pixels of vertical chrome that pushed the
  // tabs and [+] button down inside the 24-px bar, even with both
  // scrollbar policies set to AlwaysOff. Without it, overflowing tabs
  // simply squish via QHBoxLayout's default behavior.
  //
  //   auto* tabs_scroll = new HorizontalScrollArea(tabs_bar_widget);
  //   tabs_scroll->setObjectName("plotTabsScrollArea");
  //   tabs_scroll->setFrameShape(QFrame::NoFrame);
  //   tabs_scroll->setWidgetResizable(true);
  //   tabs_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  //   tabs_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  //   tabs_scroll->setFixedHeight(kTabBarHeight);
  //   tabs_scroll->setWidget(tabs_inner);
  //   outer_layout->addWidget(tabs_scroll, 1);
  tabs_inner_ = new QWidget(tabs_bar_widget);
  tabs_inner_->setObjectName("plotTabsInner");
  tabs_inner_->setContentsMargins(0, 0, 0, 0);
  tabs_inner_->setFixedHeight(kTabBarHeight);
  tabs_bar_layout_ = new QHBoxLayout(tabs_inner_);
  tabs_bar_layout_->setContentsMargins(0, 0, 0, 0);
  tabs_bar_layout_->setSpacing(0);

  button_add_tab_ = new QPushButton(this);
  button_add_tab_->setFlat(true);
  button_add_tab_->setFixedSize(QSize(kTabBarButtonSize, kTabBarButtonSize));
  button_add_tab_->setIconSize(QSize(kTabBarIconSize, kTabBarIconSize));
  button_add_tab_->setFocusPolicy(Qt::NoFocus);
  connect(button_add_tab_, &QPushButton::clicked, this, &TabbedPlotWidget::onAddTabButtonPressed);
  tabs_bar_layout_->addWidget(button_add_tab_, 0, Qt::AlignVCenter);
  tabs_bar_layout_->addStretch(1);

  outer_layout->addWidget(tabs_inner_, 1);

  // Panel-toggle buttons control the surrounding shell panels (left
  // column, timeline, right toolbar). Created here so the accessors and
  // the MainWindow wiring keep working, but NOT mounted in the tab
  // strip: the shell reparents them into the title bar's right cluster
  // (TitleBar::addRightClusterWidget) after construction.
  auto make_panel_button = [this](const char* tip) {
    auto* button = new QPushButton(this);
    button->setObjectName(QStringLiteral("plotTabsPanelButton"));
    button->setFlat(true);
    // Not checkable — MainWindow swaps the glyph between filled (panel
    // visible) and outlined (panel hidden) variants on click.
    button->setFixedSize(QSize(kTabBarButtonSize, kTabBarButtonSize));
    button->setIconSize(QSize(kTabBarIconSize, kTabBarIconSize));
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolTip(QObject::tr(tip));
    return button;
  };
  button_left_panel_ = make_panel_button(QT_TR_NOOP("Toggle left panel"));
  button_bottom_panel_ = make_panel_button(QT_TR_NOOP("Toggle bottom panel"));
  button_right_panel_ = make_panel_button(QT_TR_NOOP("Toggle right panel"));

  root_layout->addWidget(tabs_bar_widget);

  // 1px separator — same paint pattern as the title-bar / timeline /
  // right-toolbar dividers so all chrome lines look identical.
  auto* tabs_separator = new QFrame(this);
  tabs_separator->setObjectName("plotTabsSeparator");
  tabs_separator->setFrameShape(QFrame::NoFrame);
  tabs_separator->setAutoFillBackground(true);
  tabs_separator->setFixedHeight(1);
  root_layout->addWidget(tabs_separator);

  stack_ = new QStackedWidget(this);
  stack_->setContentsMargins(0, 0, 0, 0);
  root_layout->addWidget(stack_, 1);

  onStylesheetChanged(currentTheme());
  addTab({});
}

TabbedPlotWidget::~TabbedPlotWidget() = default;

PlotDocker* TabbedPlotWidget::currentTab() {
  return qobject_cast<PlotDocker*>(stack_->currentWidget());
}

int TabbedPlotWidget::dockerCount() const {
  return static_cast<int>(tabs_.size());
}

PlotDocker* TabbedPlotWidget::dockerAt(int index) {
  if (index < 0 || index >= dockerCount()) {
    return nullptr;
  }
  return tabs_[static_cast<std::size_t>(index)].docker;
}

PlotDocker* TabbedPlotWidget::addTab(QString tab_name) {
  if (tab_name.isEmpty()) {
    tab_name = QString("tab%1").arg(++tab_suffix_count_);
  }
  PlotDocker* docker = createDocker(tab_name);
  PlotTabFrame* frame = createTabFrame(tab_name, docker);

  stack_->addWidget(docker);
  // Insert frame just before the add-button (which is at index 0 after
  // the stretch we appended) so tabs grow leftward of the [+] button.
  // Explicit AlignVCenter — same rationale as the [+] button: matches
  // the 1-px-breathing-room rhythm of the rest of the chrome bars.
  const int insert_index = static_cast<int>(tabs_.size());
  tabs_bar_layout_->insertWidget(insert_index, frame, 0, Qt::AlignVCenter);
  tabs_.push_back({frame, docker});

  emit tabAdded(docker);

  stack_->setCurrentWidget(docker);
  updateSelectionStyle();
  emit currentTabChanged(docker);
  return docker;
}

void TabbedPlotWidget::setDataServices(SessionManager* session, CatalogModel* catalog) {
  session_ = session;
  catalog_ = catalog;
  for (const TabEntry& entry : tabs_) {
    entry.docker->setDataServices(session_, catalog_);
  }
}

void TabbedPlotWidget::setObjectWidgetFactory(ObjectWidgetFactory factory) {
  object_widget_factory_ = std::move(factory);
  for (const TabEntry& entry : tabs_) {
    entry.docker->setObjectWidgetFactory(object_widget_factory_);
  }
}

void TabbedPlotWidget::onAddTabButtonPressed() {
  addTab({});
  emit undoableChange();
}

void TabbedPlotWidget::onTabFrameClicked(PlotTabFrame* frame) {
  if (TabEntry* entry = findEntry(frame)) {
    stack_->setCurrentWidget(entry->docker);
    updateSelectionStyle();
    emit currentTabChanged(entry->docker);
  }
}

void TabbedPlotWidget::onTabRenameRequested(PlotTabFrame* frame, const QString& new_name) {
  TabEntry* entry = findEntry(frame);
  if (entry == nullptr) {
    return;
  }
  entry->docker->setName(new_name);
  emit undoableChange();
}

void TabbedPlotWidget::onTabCloseRequested(PlotTabFrame* frame) {
  TabEntry* entry = findEntry(frame);
  if (entry == nullptr) {
    return;
  }
  // Always keep at least one tab open.
  if (tabs_.size() == 1) {
    onAddTabButtonPressed();
    entry = findEntry(frame);  // vector reallocated above
    if (entry == nullptr) {
      return;
    }
  }

  PlotDocker* docker = entry->docker;
  PlotTabFrame* frame_widget = entry->frame;

  tabs_.erase(tabs_.begin() + (entry - tabs_.data()));

  stack_->removeWidget(docker);
  tabs_bar_layout_->removeWidget(frame_widget);
  frame_widget->deleteLater();
  docker->deleteLater();

  if (stack_->currentWidget() == nullptr && !tabs_.empty()) {
    stack_->setCurrentWidget(tabs_.front().docker);
  }
  updateSelectionStyle();
  emit currentTabChanged(qobject_cast<PlotDocker*>(stack_->currentWidget()));
  emit undoableChange();
}

TabbedPlotWidget::TabEntry* TabbedPlotWidget::findEntry(PlotTabFrame* frame) {
  for (TabEntry& entry : tabs_) {
    if (entry.frame == frame) {
      return &entry;
    }
  }
  return nullptr;
}

TabbedPlotWidget::TabEntry* TabbedPlotWidget::findEntry(PlotDocker* docker) {
  for (TabEntry& entry : tabs_) {
    if (entry.docker == docker) {
      return &entry;
    }
  }
  return nullptr;
}

void TabbedPlotWidget::updateSelectionStyle() {
  auto* current = qobject_cast<PlotDocker*>(stack_->currentWidget());
  for (const TabEntry& entry : tabs_) {
    entry.frame->setSelected(entry.docker == current);
  }
}

PlotDocker* TabbedPlotWidget::createDocker(const QString& tab_name) {
  auto* docker = new PlotDocker(tab_name, session_, catalog_, this);
  docker->setObjectWidgetFactory(object_widget_factory_);
  connect(docker, &PlotDocker::undoableChange, this, &TabbedPlotWidget::undoableChange);
  return docker;
}

PlotTabFrame* TabbedPlotWidget::createTabFrame(const QString& tab_name, PlotDocker* docker) {
  auto* frame = new PlotTabFrame(tab_name, this);
  // Tab frames default to kTabBarButtonSize; rebind to the live chrome
  // extent so a tab added after the user customised icon metrics still
  // fills the bar.
  const int chrome_extent = std::max(
      1, (chrome_metrics_.icon_size + chrome_metrics_.icon_padding) - 1 + (2 * chrome_metrics_.layout_padding));
  frame->setFixedHeight(chrome_extent);
  frame->closeButton()->setIcon(LoadSvg(":/resources/svg/close-button.svg", currentTheme()));
  connect(frame, &PlotTabFrame::clicked, this, [this, frame]() { onTabFrameClicked(frame); });
  connect(frame, &PlotTabFrame::renameCommitted, this, [this, frame](const QString& new_name) {
    onTabRenameRequested(frame, new_name);
  });
  connect(frame, &PlotTabFrame::closeRequested, this, [this, frame]() { onTabCloseRequested(frame); });
  // Suppress the unused-warning-on-no-capture by acknowledging docker.
  (void)docker;
  return frame;
}

void TabbedPlotWidget::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  chrome_metrics_ = metrics;
  // The tab strip historically uses (icon + icon_padding − 1) for the
  // strip height and chrome buttons, plus a 1-px separator below for
  // alignment with full-extent left-column bands. Layout padding adds
  // on top, growing the strip uniformly.
  const int chrome_extent = std::max(1, (metrics.icon_size + metrics.icon_padding) - 1 + (2 * metrics.layout_padding));
  if (tabs_inner_ != nullptr) {
    tabs_inner_->setFixedHeight(chrome_extent);
  }
  const int button_extent = std::max(1, chrome_extent);
  const QSize icon_sz(metrics.icon_size, metrics.icon_size);
  const std::array<QPushButton*, 4> chrome_buttons{
      button_add_tab_, button_left_panel_, button_bottom_panel_, button_right_panel_};
  for (QPushButton* btn : chrome_buttons) {
    if (btn == nullptr) {
      continue;
    }
    btn->setFixedSize(QSize(button_extent, button_extent));
    btn->setIconSize(icon_sz);
  }
  for (const TabEntry& entry : tabs_) {
    if (entry.frame != nullptr) {
      entry.frame->setFixedHeight(chrome_extent);
    }
  }
}

void TabbedPlotWidget::onStylesheetChanged(QString theme) {
  if (button_add_tab_ != nullptr) {
    button_add_tab_->setIcon(LoadSvg(":/resources/svg/add_tab.svg", theme));
  }
  // Panel-toggle button icons are owned by MainWindow because their
  // open/close glyph depends on the live visibility of the target
  // panel — knowledge this widget intentionally doesn't have.
  const QIcon close_icon = LoadSvg(":/resources/svg/close-button.svg", theme);
  for (const TabEntry& entry : tabs_) {
    if (auto* close_btn = entry.frame->closeButton()) {
      close_btn->setIcon(close_icon);
    }
    entry.docker->onStylesheetChanged(theme);
  }
}

QDomElement TabbedPlotWidget::xmlSaveState(QDomDocument& doc) const {
  QDomElement tabbed_area = doc.createElement(QStringLiteral("tabbed_widget"));
  tabbed_area.setAttribute(QStringLiteral("id"), state_id_);
  tabbed_area.setAttribute(QStringLiteral("name"), name_);
  tabbed_area.setAttribute(QStringLiteral("parent"), QStringLiteral("main_window"));

  for (const TabEntry& entry : tabs_) {
    QDomElement tab_element = entry.docker->xmlSaveState(doc);
    tab_element.setAttribute(QStringLiteral("tab_name"), entry.docker->name());
    tabbed_area.appendChild(tab_element);
  }

  PlotDocker* current = qobject_cast<PlotDocker*>(stack_->currentWidget());
  int current_index = 0;
  for (std::size_t i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].docker == current) {
      current_index = static_cast<int>(i);
      break;
    }
  }
  QDomElement current_tab = doc.createElement(QStringLiteral("currentTabIndex"));
  current_tab.setAttribute(QStringLiteral("index"), current_index);
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

  // Tear down existing tabs (frames + dockers) before rebuilding.
  for (TabEntry& entry : tabs_) {
    stack_->removeWidget(entry.docker);
    entry.docker->deleteLater();
    tabs_bar_layout_->removeWidget(entry.frame);
    entry.frame->deleteLater();
  }
  tabs_.clear();
  tab_suffix_count_ = 0;

  for (qsizetype target_index = 0; target_index < target_tabs.size(); ++target_index) {
    const QDomElement tab_element = target_tabs.at(target_index);
    const QString tab_name =
        tab_element.attribute(QStringLiteral("tab_name"), QStringLiteral("tab%1").arg(target_index + 1));
    PlotDocker* docker = addTab(tab_name);
    docker->setStateId(tab_element.attribute(QStringLiteral("id")));
    docker->setName(tab_name);
    if (!docker->xmlLoadState(tab_element)) {
      restoring_state_ = false;
      return false;
    }
  }

  const int requested_index = tabbed_area.firstChildElement(QStringLiteral("currentTabIndex"))
                                  .attribute(QStringLiteral("index"), QStringLiteral("0"))
                                  .toInt();
  const int max_index = dockerCount() - 1;
  const int current_index = std::clamp(requested_index, 0, std::max(0, max_index));
  if (PlotDocker* docker = dockerAt(current_index)) {
    stack_->setCurrentWidget(docker);
    updateSelectionStyle();
    emit currentTabChanged(docker);
  }
  restoring_state_ = false;
  return true;
}

}  // namespace PJ

#include "TabbedPlotWidget.moc"
