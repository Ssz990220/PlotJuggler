#pragma once

#include <QWidget>

QT_BEGIN_NAMESPACE
class QPushButton;
class QTabWidget;
QT_END_NAMESPACE

namespace PJ {

class CatalogModel;
class PlotDocker;
class SessionManager;

// QTabWidget with one PlotDocker per tab and a floating "+" button.
class TabbedPlotWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TabbedPlotWidget(QWidget* parent = nullptr);
  explicit TabbedPlotWidget(QString name, QWidget* parent = nullptr);
  ~TabbedPlotWidget() override;

  PlotDocker* currentTab();
  QTabWidget* tabWidget() {
    return tab_widget_;
  }
  const QTabWidget* tabWidget() const {
    return tab_widget_;
  }
  PlotDocker* addTab(QString name);
  void setDataServices(SessionManager* session, CatalogModel* catalog);

  QString name() const {
    return name_;
  }

 public slots:
  void onStylesheetChanged(QString theme);

 signals:
  void undoableChange();
  void tabAdded(PlotDocker* docker);

 private slots:
  void onRenameCurrentTab();
  void onAddTabButtonPressed();
  void onTabWidgetCurrentChanged(int index);
  void onTabCloseRequested(int index);

 protected:
  void paintEvent(QPaintEvent* event) override;
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  QTabWidget* tab_widget_ = nullptr;
  QPushButton* button_add_tab_ = nullptr;
  SessionManager* session_ = nullptr;
  CatalogModel* catalog_ = nullptr;
  QString name_;
  int tab_suffix_count_ = 0;
};

}  // namespace PJ
