#pragma once

#include <QWidget>

namespace Ui {
class CurveListPanel;
}

namespace PJ {

class CatalogModel;
class CurveTreeView;

// GREEN region: the Timeseries List tree with filter / values toggle /
// trash, over a Custom Series section. Trimmed port of PJ3
// curvelist_panel.{h,cpp,ui}: the tree subclass (CurveTreeView) is ported
// and preserves PJ3's mime-type contract, but value refresh and custom
// transforms are stubbed pending CatalogModel / TransformRegistry wiring.
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
  void clearAllCurvesRequested();

 private slots:
  void onFilterChanged(const QString& text);
  void onCustomFilterChanged(const QString& text);
  void onShowValuesToggled(bool show);
  void onTrashClicked();

 private:
  void onCurveAdded(const QString& name);
  void onCurveRemoved(const QString& name);
  void onCatalogCleared();

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
};

}  // namespace PJ
