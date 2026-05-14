#include "pj_plotting/CurveEditor.h"

#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPen>
#include <QPoint>
#include <QPushButton>
#include <QString>
#include <QToolButton>
#include <QWidgetAction>

#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_widgets/ColorPickerPopup.h"
#include "pj_widgets/ElidingLabel.h"
#include "pj_widgets/SvgUtil.h"
#include "ui_CurveEditor.h"

namespace PJ {

namespace {

// Inline-row geometry. Every inner widget sits flush with the row's
// outer border — no internal padding. The swatch is a square anchored
// to the left, eye + trash are squares anchored to the right, all
// scaling to the row's height so they maintain 1:1 aspect. The name
// takes whatever horizontal space is left.
constexpr int kRowHeight = 20;
constexpr int kRowSpacing = 2;

constexpr auto kCurveNameRole = Qt::UserRole;

constexpr auto kVisibilityOnPath = ":/resources/svg/visibility.svg";
constexpr auto kVisibilityOffPath = ":/resources/svg/visibility_off.svg";
constexpr auto kTrashIconPath = ":/resources/svg/trash.svg";

// Property keys tagged onto per-row QToolButtons so onStylesheetChanged
// can find them via findChildren and re-tint without rebuilding rows.
constexpr auto kVisibilityButtonProperty = "pj.curveEditor.visibilityButton";
constexpr auto kTrashButtonProperty = "pj.curveEditor.trashButton";

// Stylesheet for the per-row color swatch. Borderless sharp-edged square —
// the fill alone advertises the curve color, pointing-hand cursor signals
// clickability.
[[nodiscard]] QString swatchStyleSheet(QColor color) {
  return QStringLiteral("background-color: %1; border: none; border-radius: 0;").arg(color.name());
}

// Custom row widget. Uses explicit geometry instead of a QHBoxLayout so
// the eye + trash icons pin to the right edge regardless of available
// width — they never get pushed out, no jitter while resizing, and the
// name takes the leftover middle (or hides). The swatch is left-anchored,
// the buttons are right-anchored, the name fills (or vanishes from) the
// gap between them.
class CurveRowWidget : public QWidget {
 public:
  CurveRowWidget(QPushButton* swatch, ElidingLabel* name, QToolButton* eye, QToolButton* trash, QWidget* parent)
      : QWidget(parent), swatch_(swatch), name_(name), eye_(eye), trash_(trash) {
    swatch_->setParent(this);
    name_->setParent(this);
    eye_->setParent(this);
    trash_->setParent(this);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
  }

  [[nodiscard]] QSize sizeHint() const override {
    // Width is whatever the listWidget viewport gives us; the row's
    // height stays fixed so QListWidget sizes the item consistently.
    return {0, kRowHeight};
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    const int h = height();
    const int total_w = width();

    // Every inner widget is a square of side `h` (the row height), so
    // each one is 1:1 and flush with the top + bottom borders. The
    // swatch anchors left, the two action buttons anchor right.
    swatch_->setGeometry(0, 0, h, h);

    const int trash_x = total_w - h;
    trash_->setGeometry(trash_x, 0, h, h);
    const int eye_x = trash_x - kRowSpacing - h;
    eye_->setGeometry(eye_x, 0, h, h);

    // Name lives between the swatch and the eye. If the gap is too
    // small to be readable, hide it — eye + trash stay put.
    const int name_x = h + kRowSpacing;
    const int name_right = eye_x - kRowSpacing;
    const int name_width = name_right - name_x;
    if (name_width < 1) {
      if (name_->isVisible()) {
        name_->setVisible(false);
      }
      return;
    }
    if (!name_->isVisible()) {
      name_->setVisible(true);
    }
    name_->setGeometry(name_x, 0, name_width, h);
  }

 private:
  QPushButton* swatch_;
  ElidingLabel* name_;
  QToolButton* eye_;
  QToolButton* trash_;
};

}  // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent), ui_(new Ui::CurveEditor) {
  ui_->setupUi(this);

  // Curves header: filter line edit + kebab menu, mirroring the Datasets
  // header. The kebab currently hosts a single "Clear all curves" action;
  // future panel-level options (sort, hide-invisible, etc.) hang here.
  auto* curves_menu = new QMenu(this);
  curves_menu->setObjectName(QStringLiteral("PJMenu"));
  auto* clear_all_button = new QPushButton(tr("Clear all curves"), curves_menu);
  clear_all_button->setFlat(true);
  clear_all_button->setProperty("destructive", true);
  clear_all_button->setIcon(LoadSvg(":/resources/svg/trash.svg", current_theme_));
  connect(clear_all_button, &QPushButton::clicked, this, [this, curves_menu]() {
    curves_menu->hide();
    if (plot_ == nullptr) {
      return;
    }
    // Snapshot curve names first — removeCurve fires curveListChanged which
    // mutates the underlying list, so we can't iterate it live.
    QStringList names;
    for (const auto& info : plot_->curveList()) {
      if (info.curve != nullptr) {
        names << info.curve->title().text();
      }
    }
    for (const QString& name : names) {
      plot_->removeCurve(name);
    }
    plot_->replot();
    emit plot_->undoableChange();
  });
  auto* clear_all_action = new QWidgetAction(curves_menu);
  clear_all_action->setDefaultWidget(clear_all_button);
  curves_menu->addAction(clear_all_action);

  connect(ui_->buttonCurvesMenu, &QToolButton::clicked, this, [this, curves_menu]() {
    // Right-align the popup with the kebab: open at the button's bottom-
    // left, then shift left by (menu_width - button_width) so the menu's
    // right edge sits flush with the button's right edge. menu->width()
    // is only meaningful after popup() lays out, so we reposition after
    // the initial show — popup() is non-blocking.
    auto* btn = ui_->buttonCurvesMenu;
    const QPoint bottom_left = btn->mapToGlobal(QPoint(0, btn->height()));
    curves_menu->popup(bottom_left);
    const int shift = curves_menu->width() - btn->width();
    if (shift > 0) {
      curves_menu->move(bottom_left.x() - shift, bottom_left.y());
    }
  });
  connect(ui_->lineEditCurvesFilter, &QLineEdit::textChanged, this, &CurveEditor::onFilterChanged);
  // Enter while typing drops focus — restores any sibling header chrome
  // via the focus-out branch of eventFilter without a stray click.
  connect(ui_->lineEditCurvesFilter, &QLineEdit::returnPressed, ui_->lineEditCurvesFilter, &QLineEdit::clearFocus);
  ui_->lineEditCurvesFilter->installEventFilter(this);
  // Lock the header row to its natural height so hiding label/kebab on
  // filter focus doesn't shift the line edit vertically.
  ui_->widgetLabelCurves->layout()->activate();
  ui_->widgetLabelCurves->setFixedHeight(ui_->widgetLabelCurves->layout()->sizeHint().height());

  // Progressive collapse runs off CurveEditor::resizeEvent (our own
  // width), not off the header band's QWidget — the band can't shrink
  // below the sum of its content's natural sizes, so it would never
  // trigger a hide threshold on its own.
  ui_->listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Adjust mode tells the view to re-issue per-item geometry on resize
  // (otherwise items keep their first-laid-out width and the rows never
  // shrink with the viewport).
  ui_->listWidget->setResizeMode(QListView::Adjust);
  // Rows sit flush against each other — no inter-item gap.
  ui_->listWidget->setSpacing(0);

  // QHBoxLayout's minimumSize is the sum of every child's natural
  // minimum. The QLabel ("Curves") and the QLineEdit have non-zero
  // implicit minimums, which would keep the header band — and the
  // CurveEditor as a whole — from ever shrinking past ~96 px. Zero
  // them out so the layout can actually collapse and our resizeEvent
  // threshold has something to act on.
  setMinimumWidth(0);
  ui_->widgetLabelCurves->setMinimumWidth(0);
  ui_->labelCurves->setMinimumWidth(0);
  ui_->lineEditCurvesFilter->setMinimumWidth(0);
  ui_->listWidget->setMinimumWidth(0);
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
    return;
  }

  for (const auto& info : plot_->curveList()) {
    if (info.curve == nullptr) {
      continue;
    }
    appendRow(info.curve->title().text(), info.curve->pen().color(), info.curve->isVisible());
  }
  // Preserve the active filter across refreshes.
  applyFilter();
}

void CurveEditor::appendRow(const QString& curve_name, QColor color, bool visible) {
  auto* item = new QListWidgetItem();
  item->setData(kCurveNameRole, curve_name);

  // Children are constructed without a parent — CurveRowWidget's ctor
  // reparents them in one place so resizeEvent can pin geometry directly
  // (sizes scale to the row height, so no setFixedSize here).
  auto* swatch = new QPushButton();
  swatch->setCursor(Qt::PointingHandCursor);
  swatch->setFlat(true);
  swatch->setStyleSheet(swatchStyleSheet(color));
  connect(swatch, &QPushButton::clicked, this, [this, curve_name, swatch]() { onSwatchClicked(curve_name, swatch); });

  auto* visibility = new QToolButton();
  // The objectName drives the curveVisibilityToggle QSS rule that strips
  // QToolButton's default hover / checked background so the eye icon
  // appears as a plain ink glyph regardless of state.
  visibility->setObjectName(QStringLiteral("curveVisibilityToggle"));
  visibility->setProperty(kVisibilityButtonProperty, curve_name);
  visibility->setCheckable(true);
  visibility->setAutoRaise(true);
  visibility->setFocusPolicy(Qt::NoFocus);
  visibility->setIconSize(QSize(kRowHeight, kRowHeight));
  visibility->setChecked(visible);
  visibility->setIcon(LoadSvg(visible ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
  visibility->setToolTip(tr("Toggle curve visibility"));
  connect(visibility, &QToolButton::toggled, this, [this, curve_name, visibility](bool checked) {
    visibility->setIcon(LoadSvg(checked ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
    onVisibilityToggled(curve_name, checked);
  });

  auto* name_label = new ElidingLabel();
  name_label->setObjectName(QStringLiteral("curveNameLabel"));
  // Curve names are typically topic paths (`/foo/bar/leaf`); elide from
  // the left so the meaningful leaf stays visible as the row narrows.
  name_label->setElideMode(Qt::ElideLeft);
  name_label->setFullText(curve_name);

  auto* trash = new QToolButton();
  trash->setObjectName(QStringLiteral("curveTrashToggle"));
  trash->setProperty(kTrashButtonProperty, curve_name);
  trash->setAutoRaise(true);
  trash->setFocusPolicy(Qt::NoFocus);
  trash->setIconSize(QSize(kRowHeight, kRowHeight));
  trash->setIcon(LoadSvg(kTrashIconPath, current_theme_));
  trash->setToolTip(tr("Remove this curve from its plot"));
  connect(trash, &QToolButton::clicked, this, [this, curve_name]() {
    if (plot_ == nullptr) {
      return;
    }
    plot_->removeCurve(curve_name);
    plot_->replot();
    emit plot_->undoableChange();
  });

  auto* row_widget = new CurveRowWidget(swatch, name_label, visibility, trash, /*parent=*/nullptr);
  ui_->listWidget->addItem(item);
  item->setSizeHint(QSize(0, kRowHeight));
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

void CurveEditor::onStylesheetChanged(QString theme) {
  current_theme_ = std::move(theme);
  // Header chrome icons: search glyph + kebab.
  ui_->buttonSearchCurves->setIcon(LoadSvg(":/resources/svg/search_light.svg", current_theme_));
  ui_->buttonCurvesMenu->setIcon(LoadSvg(":/resources/svg/more_vert.svg", current_theme_));
  // Re-tint every row's visibility + trash toggles to the new theme ink.
  // The buttons are owned by the row widgets stored as itemWidget on each
  // QListWidgetItem; QObject::findChildren walks that subtree.
  for (int i = 0; i < ui_->listWidget->count(); ++i) {
    QWidget* row = ui_->listWidget->itemWidget(ui_->listWidget->item(i));
    if (row == nullptr) {
      continue;
    }
    for (auto* button : row->findChildren<QToolButton*>()) {
      if (button->property(kVisibilityButtonProperty).isValid()) {
        button->setIcon(LoadSvg(button->isChecked() ? kVisibilityOnPath : kVisibilityOffPath, current_theme_));
      } else if (button->property(kTrashButtonProperty).isValid()) {
        button->setIcon(LoadSvg(kTrashIconPath, current_theme_));
      }
    }
  }
}

void CurveEditor::onFilterChanged(const QString& /*text*/) {
  applyFilter();
}

void CurveEditor::applyFilter() {
  const QString needle = ui_->lineEditCurvesFilter->text().trimmed();
  for (int i = 0; i < ui_->listWidget->count(); ++i) {
    QListWidgetItem* item = ui_->listWidget->item(i);
    const QString curve_name = item->data(kCurveNameRole).toString();
    const bool match = needle.isEmpty() || curve_name.contains(needle, Qt::CaseInsensitive);
    item->setHidden(!match);
  }
}

bool CurveEditor::eventFilter(QObject* watched, QEvent* event) {
  const QEvent::Type type = event->type();
  if ((type == QEvent::FocusIn || type == QEvent::FocusOut) && watched == ui_->lineEditCurvesFilter) {
    // Focus expands the filter into the label's space. Kebab stays
    // visible — it never gets pushed out.
    const bool focused = (type == QEvent::FocusIn);
    ui_->labelCurves->setVisible(!focused);
  }
  return QWidget::eventFilter(watched, event);
}

void CurveEditor::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateHeaderForWidth();
}

void CurveEditor::updateHeaderForWidth() {
  // When the panel narrows past the point where a usable filter would
  // fit, hide the filter + search icon. The "Curves" label stays as
  // the section title; the kebab is always visible — it never gets
  // pushed out. Filter-focus still hides the label to widen the input
  // (handled in eventFilter).
  constexpr int kFilterHideBelow = 140;
  const bool wide_enough = width() >= kFilterHideBelow;
  ui_->buttonSearchCurves->setVisible(wide_enough);
  ui_->lineEditCurvesFilter->setVisible(wide_enough);
}

}  // namespace PJ
