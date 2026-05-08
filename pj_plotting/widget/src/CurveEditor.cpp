#include "pj_plotting/CurveEditor.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPen>
#include <QPoint>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QString>

#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "ui_CurveEditor.h"

namespace PJ {

namespace {

// Inline-row layout: [swatch | visibility | name].
constexpr int kSwatchSize = 16;
constexpr int kRowSpacing = 6;
constexpr int kRowMargin = 2;

constexpr auto kCurveNameRole = Qt::UserRole;

// Maps the comboBoxWidth index (4 entries) to the actual pen width applied
// to the curve. Matches the enum values in PlotWidgetBase::LineWidth.
[[nodiscard]] double widthForComboIndex(int index) {
  switch (index) {
    case 0:
      return 1.0;
    case 1:
      return 1.5;
    case 2:
      return 2.0;
    case 3:
      return 3.0;
    default:
      return 1.0;
  }
}

// Inverse of widthForComboIndex — selects the closest combo entry to the
// curve's current pen width so syncControlsToSelectedCurve has a defined
// initial state even when the pen width was set programmatically to a value
// that doesn't match any combo entry exactly.
[[nodiscard]] int comboIndexForWidth(double width) {
  if (width >= 2.5) {
    return 3;  // 3.0
  }
  if (width >= 1.75) {
    return 2;  // 2.0
  }
  if (width >= 1.25) {
    return 1;  // 1.5
  }
  return 0;  // 1.0
}

// Stylesheet for the per-row color swatch. Border keeps the fill visible on
// either light or dark themes; pointing-hand cursor advertises clickability.
[[nodiscard]] QString swatchStyleSheet(QColor color) {
  return QStringLiteral("background-color: %1; border: 1px solid #444; border-radius: 3px;").arg(color.name());
}

}  // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent), ui_(new Ui::CurveEditor) {
  ui_->setupUi(this);

  connect(ui_->listWidget, &QListWidget::itemSelectionChanged, this, &CurveEditor::onListSelectionChanged);
  connect(
      ui_->comboBoxWidth, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this,
      &CurveEditor::onWidthChanged);

  // Each radio routes through the same slot; the slot reads the currently-
  // checked button to find the new style. Avoids one connect-per-button-per-
  // style mapping inline.
  for (QRadioButton* radio :
       {ui_->radioLines, ui_->radioDots, ui_->radioLinesAndDots, ui_->radioSticks, ui_->radioSteps,
        ui_->radioStepsInverted}) {
    connect(radio, &QRadioButton::toggled, this, [this](bool checked) {
      if (checked) {
        onStyleChanged();
      }
    });
  }

  connect(ui_->buttonDeleteCurve, &QPushButton::clicked, this, &CurveEditor::onDeleteClicked);

  setDetailControlsEnabled(false);
}

CurveEditor::~CurveEditor() {
  delete ui_;
}

void CurveEditor::setPlot(PlotWidget* plot) {
  if (plot_ == plot) {
    return;
  }
  if (curve_list_connection_) {
    QObject::disconnect(curve_list_connection_);
  }
  if (plot_destroyed_connection_) {
    QObject::disconnect(plot_destroyed_connection_);
  }
  plot_ = plot;
  clearActivePicker();
  if (plot_ != nullptr) {
    curve_list_connection_ = connect(plot_, &PlotWidgetBase::curveListChanged, this, &CurveEditor::refresh);
    plot_destroyed_connection_ = connect(plot_, &QObject::destroyed, this, [this]() {
      plot_ = nullptr;
      clearActivePicker();
      ui_->listWidget->clear();
      setDetailControlsEnabled(false);
      ui_->buttonDeleteCurve->setEnabled(false);
    });
  }
  refresh();
}

void CurveEditor::refresh() {
  // Clear without firing selection-change handlers; we re-evaluate at the end.
  QSignalBlocker block_list(ui_->listWidget);
  ui_->listWidget->clear();
  // Row swatches are about to be destroyed — invalidate any cached pointer.
  clearActivePicker();

  if (plot_ == nullptr) {
    setDetailControlsEnabled(false);
    ui_->buttonDeleteCurve->setEnabled(false);
    return;
  }

  for (const auto& info : plot_->curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    appendRow(info.curve->title().text(), info.curve->pen().color(), info.curve->isVisible());
  }

  // No row is selected after a clear → controls disabled, button greyed out.
  setDetailControlsEnabled(false);
  ui_->buttonDeleteCurve->setEnabled(false);
}

void CurveEditor::onListSelectionChanged() {
  const bool has_selection = !ui_->listWidget->selectedItems().isEmpty();
  setDetailControlsEnabled(has_selection);
  ui_->buttonDeleteCurve->setEnabled(has_selection);
  if (has_selection) {
    syncControlsToSelectedCurve();
  }
}

void CurveEditor::onWidthChanged(int combo_index) {
  if (plot_ == nullptr) {
    return;
  }
  const QString name = selectedCurveName();
  if (name.isEmpty()) {
    return;
  }
  plot_->setCurveLineWidth(name, widthForComboIndex(combo_index));
}

void CurveEditor::onStyleChanged() {
  if (plot_ == nullptr) {
    return;
  }
  const QString name = selectedCurveName();
  if (name.isEmpty()) {
    return;
  }
  PlotWidgetBase::CurveStyle style = PlotWidgetBase::kLines;
  if (ui_->radioDots->isChecked()) {
    style = PlotWidgetBase::kDots;
  } else if (ui_->radioLinesAndDots->isChecked()) {
    style = PlotWidgetBase::kLinesAndDots;
  } else if (ui_->radioSticks->isChecked()) {
    style = PlotWidgetBase::kSticks;
  } else if (ui_->radioSteps->isChecked()) {
    style = PlotWidgetBase::kSteps;
  } else if (ui_->radioStepsInverted->isChecked()) {
    style = PlotWidgetBase::kStepsInverted;
  }
  plot_->setCurveStyle(name, style);
}

void CurveEditor::onDeleteClicked() {
  if (plot_ == nullptr) {
    return;
  }
  const QString name = selectedCurveName();
  if (name.isEmpty()) {
    return;
  }
  // PlotWidget::removeCurve fires curveListChanged → refresh() rebuilds the
  // list and clears selection. The detail-pane disable is handled there.
  plot_->removeCurve(name);
  plot_->replot();
  emit plot_->undoableChange();
}

void CurveEditor::setDetailControlsEnabled(bool enabled) {
  ui_->comboBoxWidth->setEnabled(enabled);
  ui_->frameStyle->setEnabled(enabled);
}

void CurveEditor::syncControlsToSelectedCurve() {
  if (plot_ == nullptr) {
    return;
  }
  const QString name = selectedCurveName();
  if (name.isEmpty()) {
    return;
  }
  const auto* info = plot_->curveFromTitle(name);
  if (info == nullptr || info->curve == nullptr) {
    return;
  }
  const QwtPlotCurve* curve = info->curve;

  QSignalBlocker block_combo(ui_->comboBoxWidth);
  QSignalBlocker block_lines(ui_->radioLines);
  QSignalBlocker block_dots(ui_->radioDots);
  QSignalBlocker block_both(ui_->radioLinesAndDots);
  QSignalBlocker block_sticks(ui_->radioSticks);
  QSignalBlocker block_steps(ui_->radioSteps);
  QSignalBlocker block_steps_inv(ui_->radioStepsInverted);

  ui_->comboBoxWidth->setCurrentIndex(comboIndexForWidth(curve->pen().widthF()));
  switch (PlotWidget::qwtStyleToCurveStyle(curve)) {
    case PlotWidgetBase::kLines:
      ui_->radioLines->setChecked(true);
      break;
    case PlotWidgetBase::kDots:
      ui_->radioDots->setChecked(true);
      break;
    case PlotWidgetBase::kLinesAndDots:
      ui_->radioLinesAndDots->setChecked(true);
      break;
    case PlotWidgetBase::kSticks:
      ui_->radioSticks->setChecked(true);
      break;
    case PlotWidgetBase::kSteps:
      ui_->radioSteps->setChecked(true);
      break;
    case PlotWidgetBase::kStepsInverted:
      ui_->radioStepsInverted->setChecked(true);
      break;
  }
}

void CurveEditor::appendRow(const QString& curve_name, QColor color, bool visible) {
  auto* item = new QListWidgetItem();
  item->setData(kCurveNameRole, curve_name);

  auto* row_widget = new QWidget();
  auto* layout = new QHBoxLayout(row_widget);
  layout->setContentsMargins(kRowMargin, kRowMargin, kRowMargin, kRowMargin);
  layout->setSpacing(kRowSpacing);

  auto* swatch = new QPushButton(row_widget);
  swatch->setFixedSize(kSwatchSize, kSwatchSize);
  swatch->setCursor(Qt::PointingHandCursor);
  swatch->setFlat(true);
  swatch->setStyleSheet(swatchStyleSheet(color));
  swatch->setToolTip(tr("Click to change curve color"));
  connect(swatch, &QPushButton::clicked, this, [this, curve_name, swatch]() { onSwatchClicked(curve_name, swatch); });

  auto* visibility = new QCheckBox(row_widget);
  visibility->setChecked(visible);
  visibility->setToolTip(tr("Toggle curve visibility"));
  connect(visibility, &QCheckBox::toggled, this, [this, curve_name](bool checked) {
    onVisibilityToggled(curve_name, checked);
  });

  auto* name_label = new QLabel(curve_name, row_widget);

  layout->addWidget(swatch);
  layout->addWidget(visibility);
  layout->addWidget(name_label);
  layout->addStretch(1);

  ui_->listWidget->addItem(item);
  item->setSizeHint(row_widget->sizeHint());
  ui_->listWidget->setItemWidget(item, row_widget);
}

void CurveEditor::onSwatchClicked(const QString& curve_name, QPushButton* swatch) {
  if (plot_ == nullptr) {
    return;
  }
  const auto* info = plot_->curveFromTitle(curve_name);
  const QColor current = (info != nullptr && info->curve != nullptr) ? info->curve->pen().color() : QColor(Qt::white);

  active_color_curve_ = curve_name;
  active_color_swatch_ = swatch;

  if (color_picker_ == nullptr) {
    color_picker_ = new ColorPickerPopup(this);
    connect(color_picker_, &ColorPickerPopup::colorChanged, this, &CurveEditor::onPickerColorChanged);
  }
  color_picker_->setColor(current);
  color_picker_->move(swatch->mapToGlobal(QPoint(0, swatch->height())));
  color_picker_->show();
}

void CurveEditor::onPickerColorChanged(QColor color) {
  if (plot_ == nullptr || active_color_curve_.isEmpty() || !color.isValid()) {
    return;
  }
  // onChangeCurveColor calls replot() internally; no second replot here.
  plot_->onChangeCurveColor(active_color_curve_, color);
  if (active_color_swatch_ != nullptr) {
    active_color_swatch_->setStyleSheet(swatchStyleSheet(color));
  }
  emit plot_->undoableChange();
}

void CurveEditor::clearActivePicker() {
  active_color_curve_.clear();
  active_color_swatch_ = nullptr;
  if (color_picker_ != nullptr && color_picker_->isVisible()) {
    color_picker_->hide();
  }
}

void CurveEditor::onVisibilityToggled(const QString& curve_name, bool visible) {
  if (plot_ == nullptr) {
    return;
  }
  plot_->setCurveVisible(curve_name, visible);
}

QString CurveEditor::selectedCurveName() const {
  const QList<QListWidgetItem*> selected = ui_->listWidget->selectedItems();
  if (selected.isEmpty()) {
    return {};
  }
  return selected.first()->data(kCurveNameRole).toString();
}

}  // namespace PJ
