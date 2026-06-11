#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_widgets/ChromeMetrics.h"

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
class IDataWidget;
struct ObjectDropSeed;
class PlotDocker;
class PlotTabFrame;
class SessionManager;

// Custom tab strip + QStackedWidget. Each tab is a small QFrame
// containing the tab name label and a close button; clicking a frame
// switches the stack to its PlotDocker, double-clicking renames it.
// Same external API the previous QTabWidget-based version exposed.
class TabbedPlotWidget : public QWidget {
  Q_OBJECT
 public:
  // Single object-widget factory used by both drop (seed != null) and layout
  // restore (kind tag, seed == null). See DockWidget::ObjectWidgetFactory.
  using ObjectWidgetFactory =
      std::function<IDataWidget*(const QString& kind, const ObjectDropSeed* seed, QWidget* parent)>;

  explicit TabbedPlotWidget(QWidget* parent = nullptr);
  explicit TabbedPlotWidget(QString name, QWidget* parent = nullptr);
  ~TabbedPlotWidget() override;

  PlotDocker* currentTab();
  PlotDocker* addTab(QString name);
  void setDataServices(SessionManager* session, CatalogModel* catalog);
  void setObjectWidgetFactory(ObjectWidgetFactory factory);

  [[nodiscard]] int dockerCount() const;
  PlotDocker* dockerAt(int index);

  // Panel-toggle buttons. Created here but relocated by the MainWindow
  // shell into the title bar (it reparents them after construction).
  // The shell wires clicked() to show/hide the corresponding outer
  // panel widget and swaps the filled/outlined glyph.
  [[nodiscard]] QPushButton* leftPanelButton() const {
    return button_left_panel_;
  }
  [[nodiscard]] QPushButton* bottomPanelButton() const {
    return button_bottom_panel_;
  }
  [[nodiscard]] QPushButton* rightPanelButton() const {
    return button_right_panel_;
  }

  [[nodiscard]] QString name() const {
    return name_;
  }
  void setName(QString name) {
    name_ = std::move(name);
  }

  [[nodiscard]] QString stateId() const {
    return state_id_;
  }
  void setStateId(QString id) {
    if (!id.isEmpty()) {
      state_id_ = std::move(id);
    }
  }

  // Serializes / restores the tab set. Restoration rebuilds all tabs from
  // scratch and emits no undoableChange (callers can wrap in a guarded
  // block via the restoring_state_ flag).
  [[nodiscard]] QDomElement xmlSaveState(QDomDocument& doc) const;
  bool xmlLoadState(const QDomElement& tabbed_area);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds the tab-strip Chrome metrics — the [+] tab button, the
  // three panel-toggle buttons, the strip height itself, and every
  // open tab frame. Connected to MainWindow::chromeMetricsChanged.
  // layout_spacing is ignored for now — tab frames sit flush with one
  // another by design, so introducing gaps between them would expose
  // strips of the tab-bar background.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 signals:
  void undoableChange();
  void tabAdded(PlotDocker* docker);
  // Fires after the active tab changes (user click, programmatic switch,
  // tab close, or layout restore). docker is the now-active PlotDocker or
  // nullptr if no tab is active.
  void currentTabChanged(PlotDocker* docker);

 private slots:
  void onAddTabButtonPressed();
  void onTabFrameClicked(PlotTabFrame* frame);
  void onTabRenameRequested(PlotTabFrame* frame, const QString& new_name);
  void onTabCloseRequested(PlotTabFrame* frame);

 private:
  struct TabEntry {
    PlotTabFrame* frame;
    PlotDocker* docker;
  };

  TabEntry* findEntry(PlotTabFrame* frame);
  TabEntry* findEntry(PlotDocker* docker);
  void updateSelectionStyle();
  PlotDocker* createDocker(const QString& tab_name);
  PlotTabFrame* createTabFrame(const QString& tab_name, PlotDocker* docker);

  QHBoxLayout* tabs_bar_layout_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  QWidget* tabs_inner_ = nullptr;
  QPushButton* button_add_tab_ = nullptr;
  QPushButton* button_left_panel_ = nullptr;
  QPushButton* button_bottom_panel_ = nullptr;
  QPushButton* button_right_panel_ = nullptr;
  // Tab-strip Chrome metrics — defaults match the kTabBar* constants
  // in the .cpp so first paint is unchanged until the host pushes the
  // saved Preferences values. layout_spacing is ignored: tab frames sit
  // flush by design.
  ChromeMetrics chrome_metrics_{20, 3, 0, 0};
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  ObjectWidgetFactory object_widget_factory_;
  QString name_;
  QString state_id_;
  int tab_suffix_count_ = 0;
  bool restoring_state_ = false;
  std::vector<TabEntry> tabs_;
};

}  // namespace PJ
