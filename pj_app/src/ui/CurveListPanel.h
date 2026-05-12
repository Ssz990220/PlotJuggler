#pragma once

#include <QStringList>
#include <QWidget>

namespace Ui {
class CurveListPanel;
}

namespace PJ {

class CatalogModel;
class CurveTreeView;

// Timeseries list + Custom Series section. Top tree mirrors CatalogModel;
// bottom tree is the user's custom/derived series.
class CurveListPanel : public QWidget {
  Q_OBJECT
 public:
  explicit CurveListPanel(QWidget* parent = nullptr);
  ~CurveListPanel() override;

  void setCatalog(CatalogModel* catalog);

  void refreshValues(double tracker_time);

 signals:
  void createCustomSeriesRequested();
  void editCustomSeriesRequested(QString name);
  void deleteCustomSeriesRequested(QString name);
  // covers_all is true when the panel determined the selection (or its
  // empty-implies-all interpretation) targets every known curve. MainWindow
  // decides whether to prompt the user.
  void trashRequested(QStringList names, bool covers_all);

 public slots:
  void onStylesheetChanged(QString theme);

 private slots:
  void onFilterChanged(const QString& text);
  void onCustomFilterChanged(const QString& text);
  void onShowValuesToggled(bool show);
  void onTrashClicked();

 private:
  void onCurveAdded(const QString& name);
  void onCurveRemoved(const QString& name);
  void onCatalogCleared();
  void applyIcons(QString theme);

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
};

}  // namespace PJ
