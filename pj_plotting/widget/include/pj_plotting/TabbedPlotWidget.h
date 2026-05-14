#pragma once

#include <QDomDocument>
#include <QDomElement>
#include <QString>
#include <QWidget>
#include <vector>

QT_BEGIN_NAMESPACE
class QHBoxLayout;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
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
  explicit TabbedPlotWidget(QWidget* parent = nullptr);
  explicit TabbedPlotWidget(QString name, QWidget* parent = nullptr);
  ~TabbedPlotWidget() override;

  PlotDocker* currentTab();
  PlotDocker* addTab(QString name);
  void setDataServices(SessionManager* session, CatalogModel* catalog);

  [[nodiscard]] int dockerCount() const;
  PlotDocker* dockerAt(int index);

  // Panel-toggle buttons mounted at the far right of the tab strip.
  // Checkable; checked = panel visible. The MainWindow shell wires
  // toggled() to show/hide the corresponding outer panel widget.
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
  QPushButton* button_add_tab_ = nullptr;
  QPushButton* button_left_panel_ = nullptr;
  QPushButton* button_bottom_panel_ = nullptr;
  QPushButton* button_right_panel_ = nullptr;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QString name_;
  QString state_id_;
  int tab_suffix_count_ = 0;
  bool restoring_state_ = false;
  std::vector<TabEntry> tabs_;
};

}  // namespace PJ
