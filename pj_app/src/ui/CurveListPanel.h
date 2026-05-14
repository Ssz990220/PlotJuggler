#pragma once

#include <QStringList>
#include <QWidget>
#include <vector>

class QAction;
class QPushButton;

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
  void deleteCustomSeriesRequested(QString name);
  // covers_all is true when the panel determined the selection (or its
  // empty-implies-all interpretation) targets every known curve. MainWindow
  // decides whether to prompt the user.
  void trashRequested(QStringList names, bool covers_all);
  // Emitted when the user picks "Clear All" from the datasets menu.
  void clearAllCurvesRequested();

 public slots:
  void onStylesheetChanged(QString theme);

 protected:
  // Hides the sibling label / action buttons in each header band when
  // its filter QLineEdit gets focus, so the input expands to fill the
  // row. Restores them on focus loss.
  bool eventFilter(QObject* watched, QEvent* event) override;

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
  std::vector<QString> selectedCurveNamesForDrag() const;

  Ui::CurveListPanel* ui_;
  CatalogModel* catalog_ = nullptr;
  CurveTreeView* tree_view_ = nullptr;
  CurveTreeView* custom_view_ = nullptr;
  // QPushButtons hosted inside QWidgetAction items in the section
  // dropdown menus. Kept as members so applyIcons() can retint their
  // leading icons on theme switch.
  QPushButton* clear_all_button_ = nullptr;
  QPushButton* delete_custom_button_ = nullptr;
};

}  // namespace PJ
