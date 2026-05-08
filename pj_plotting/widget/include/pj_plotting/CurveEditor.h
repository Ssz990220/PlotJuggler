#pragma once

#include <QColor>
#include <QMetaObject>
#include <QString>
#include <QWidget>

#include "pj_plotting/PlotWidgetBase.h"

class QListWidgetItem;
class QPushButton;

namespace Ui {
class CurveEditor;
}

namespace PJ {

class ColorPickerPopup;
class PlotWidget;

// Side panel for editing the curves of a single PlotWidget. The top list
// shows one row per curve (color swatch + visibility checkbox + name); the
// detail pane below exposes per-curve width and style controls that operate
// on the row currently selected in the list. Per-row swatch and visibility
// changes apply immediately to their owning curve regardless of selection.
//
// Bind via setPlot(plot) / setPlot(nullptr). The panel auto-refreshes when
// the bound plot's curveListChanged() fires; a destroyed plot is detected
// via a QObject::destroyed connection.
class CurveEditor : public QWidget {
  Q_OBJECT
 public:
  explicit CurveEditor(QWidget* parent = nullptr);
  ~CurveEditor() override;

  void setPlot(PlotWidget* plot);

  [[nodiscard]] PlotWidget* plot() const noexcept {
    return plot_;
  }

 public slots:
  void refresh();

 private slots:
  void onListSelectionChanged();
  void onWidthChanged(int combo_index);
  void onStyleChanged();
  void onDeleteClicked();

 private:
  void setDetailControlsEnabled(bool enabled);
  void syncControlsToSelectedCurve();
  void appendRow(const QString& curve_name, QColor color, bool visible);
  void onSwatchClicked(const QString& curve_name, QPushButton* swatch);
  void onPickerColorChanged(QColor color);
  void onVisibilityToggled(const QString& curve_name, bool visible);
  [[nodiscard]] QString selectedCurveName() const;
  void clearActivePicker();

  Ui::CurveEditor* ui_;
  PlotWidget* plot_ = nullptr;
  QMetaObject::Connection curve_list_connection_;
  QMetaObject::Connection plot_destroyed_connection_;

  // Lazily created on first swatch click; reused for the lifetime of the
  // editor so the persistent colorChanged connection survives reuse.
  ColorPickerPopup* color_picker_ = nullptr;
  // Identifies which row's swatch the popup is currently editing. Cleared
  // whenever the row is destroyed (refresh / setPlot / plot teardown).
  QString active_color_curve_;
  QPushButton* active_color_swatch_ = nullptr;
};

}  // namespace PJ
