// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plugins/host_qt/widget_adapters.hpp"

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/DualOptionsWidget.h>
#include <pj_widgets/Scrollbar.h>
#include <pj_widgets/ToggleSwitch.h>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLayout>
#include <QPointer>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVariant>
#include <algorithm>

namespace PJ {

namespace {

// Marker properties linking an adapted original to its styled replacement.
constexpr const char* kDualOptionsWidgetProperty = "_pj_dual_options_widget";
constexpr const char* kDualOptionsIndexProperty = "_pj_dual_options_index";
constexpr const char* kDualOptionsRadio0Property = "_pj_dual_options_radio0";
constexpr const char* kDualOptionsRadio1Property = "_pj_dual_options_radio1";
constexpr const char* kDualOptionsDesiredVisibleProperty = "_pj_dual_options_desired_visible";

static PJ::DualOptionsWidget* pairedDualOptionsWidget(const QWidget* widget) {
  QObject* obj = widget->property(kDualOptionsWidgetProperty).value<QObject*>();
  return qobject_cast<PJ::DualOptionsWidget*>(obj);
}

static QRadioButton* pairedRadio(const PJ::DualOptionsWidget* dual, const char* property_name) {
  QObject* obj = dual->property(property_name).value<QObject*>();
  return qobject_cast<QRadioButton*>(obj);
}

static void syncDualOptionsFromRadioPair(QRadioButton* radio) {
  auto* dual = pairedDualOptionsWidget(radio);
  if (dual == nullptr) {
    return;
  }
  auto* first = pairedRadio(dual, kDualOptionsRadio0Property);
  auto* second = pairedRadio(dual, kDualOptionsRadio1Property);
  if (first == nullptr || second == nullptr) {
    return;
  }

  first->hide();
  second->hide();

  dual->setEnabled(first->isEnabled() && second->isEnabled());
  const bool first_visible = first->property(kDualOptionsDesiredVisibleProperty).toBool();
  const bool second_visible = second->property(kDualOptionsDesiredVisibleProperty).toBool();
  dual->setVisible(first_visible || second_visible);

  const int selected = second->isChecked() ? 1 : 0;
  const QSignalBlocker blocker(dual);
  dual->setSelectedIndex(selected);
}

static bool boxSegmentContainsOnlyPairOrSpacer(QBoxLayout* layout, QRadioButton* first, QRadioButton* second) {
  const int first_index = layout->indexOf(first);
  const int second_index = layout->indexOf(second);
  if (first_index < 0 || second_index < 0) {
    return false;
  }
  const int begin = std::min(first_index, second_index);
  const int end = std::max(first_index, second_index);
  for (int i = begin; i <= end; ++i) {
    QLayoutItem* item = layout->itemAt(i);
    if (item == nullptr || item->spacerItem() != nullptr) {
      continue;
    }
    if (item->layout() != nullptr) {
      return false;
    }
    QWidget* widget = item->widget();
    if (widget == nullptr || widget == first || widget == second) {
      continue;
    }
    return false;
  }
  return true;
}

static bool gridItemPosition(
    QGridLayout* layout, QWidget* widget, int& row, int& column, int& row_span, int& column_span) {
  const int index = layout->indexOf(widget);
  if (index < 0) {
    return false;
  }
  layout->getItemPosition(index, &row, &column, &row_span, &column_span);
  return true;
}

static bool gridSegmentContainsOnlyPairOrSpacer(
    QGridLayout* layout, QRadioButton* first, QRadioButton* second, int& row, int& column, int& column_span) {
  int first_row = 0;
  int first_col = 0;
  int first_row_span = 0;
  int first_col_span = 0;
  int second_row = 0;
  int second_col = 0;
  int second_row_span = 0;
  int second_col_span = 0;
  if (!gridItemPosition(layout, first, first_row, first_col, first_row_span, first_col_span) ||
      !gridItemPosition(layout, second, second_row, second_col, second_row_span, second_col_span)) {
    return false;
  }
  if (first_row != second_row || first_row_span != 1 || second_row_span != 1) {
    return false;
  }

  row = first_row;
  column = std::min(first_col, second_col);
  const int end_col = std::max(first_col + first_col_span, second_col + second_col_span);
  column_span = end_col - column;

  for (int i = 0; i < layout->count(); ++i) {
    int item_row = 0;
    int item_col = 0;
    int item_row_span = 0;
    int item_col_span = 0;
    layout->getItemPosition(i, &item_row, &item_col, &item_row_span, &item_col_span);
    if (item_row != row || item_col >= end_col || item_col + item_col_span <= column) {
      continue;
    }
    QLayoutItem* item = layout->itemAt(i);
    if (item == nullptr || item->spacerItem() != nullptr) {
      continue;
    }
    if (item->layout() != nullptr) {
      return false;
    }
    QWidget* widget = item->widget();
    if (widget == nullptr || widget == first || widget == second) {
      continue;
    }
    return false;
  }
  return true;
}

struct RadioPairPlacement {
  QBoxLayout* box_layout = nullptr;
  QGridLayout* grid_layout = nullptr;
  int first_index = -1;
  int second_index = -1;
  int first_column = 0;
  int second_column = 0;
  int row = 0;
  int column = 0;
  int column_span = 0;
};

static bool findRadioPairPlacement(
    QLayout* layout, QRadioButton* first, QRadioButton* second, RadioPairPlacement& placement) {
  if (layout == nullptr) {
    return false;
  }

  if (auto* box_layout = qobject_cast<QBoxLayout*>(layout)) {
    const int first_index = box_layout->indexOf(first);
    const int second_index = box_layout->indexOf(second);
    if (first_index >= 0 && second_index >= 0 && boxSegmentContainsOnlyPairOrSpacer(box_layout, first, second)) {
      placement = {};
      placement.box_layout = box_layout;
      placement.first_index = first_index;
      placement.second_index = second_index;
      return true;
    }
  }

  if (auto* grid_layout = qobject_cast<QGridLayout*>(layout)) {
    int row = 0;
    int column = 0;
    int column_span = 0;
    if (gridSegmentContainsOnlyPairOrSpacer(grid_layout, first, second, row, column, column_span)) {
      int first_row = 0;
      int first_col = 0;
      int first_row_span = 0;
      int first_col_span = 0;
      int second_row = 0;
      int second_col = 0;
      int second_row_span = 0;
      int second_col_span = 0;
      (void)gridItemPosition(grid_layout, first, first_row, first_col, first_row_span, first_col_span);
      (void)gridItemPosition(grid_layout, second, second_row, second_col, second_row_span, second_col_span);
      placement = {};
      placement.grid_layout = grid_layout;
      placement.first_column = first_col;
      placement.second_column = second_col;
      placement.row = row;
      placement.column = column;
      placement.column_span = column_span;
      return true;
    }
  }

  for (int i = 0; i < layout->count(); ++i) {
    QLayoutItem* item = layout->itemAt(i);
    if (item != nullptr && item->layout() != nullptr &&
        findRadioPairPlacement(item->layout(), first, second, placement)) {
      return true;
    }
  }
  return false;
}

static bool radioPairHasExplicitExclusiveGroup(QRadioButton* first, QRadioButton* second) {
  QButtonGroup* first_group = first->group();
  QButtonGroup* second_group = second->group();
  return first_group != nullptr && first_group == second_group && first_group->exclusive() &&
         first_group->buttons().size() == 2;
}

static bool sortRadioPairByLayout(QWidget* parent, QRadioButton*& first, QRadioButton*& second) {
  RadioPairPlacement placement;
  if (!findRadioPairPlacement(parent->layout(), first, second, placement)) {
    return false;
  }
  if (placement.box_layout != nullptr && placement.second_index < placement.first_index) {
    std::swap(first, second);
  } else if (placement.grid_layout != nullptr && placement.second_column < placement.first_column) {
    std::swap(first, second);
  }
  return true;
}

static void insertDualOptionsWidget(
    QWidget* parent, QRadioButton* first, QRadioButton* second, DualOptionsWidget* dual) {
  RadioPairPlacement placement;
  if (!findRadioPairPlacement(parent->layout(), first, second, placement)) {
    return;
  }

  if (placement.box_layout != nullptr) {
    const int insert_index = std::min(placement.first_index, placement.second_index);
    placement.box_layout->removeWidget(first);
    placement.box_layout->removeWidget(second);
    placement.box_layout->insertWidget(insert_index, dual);
  } else if (placement.grid_layout != nullptr) {
    placement.grid_layout->removeWidget(first);
    placement.grid_layout->removeWidget(second);
    placement.grid_layout->addWidget(dual, placement.row, placement.column, 1, placement.column_span);
  }
}

static bool tryAdaptRadioPair(QRadioButton* candidate_first, QRadioButton* candidate_second) {
  if (candidate_first == nullptr || candidate_second == nullptr || candidate_first == candidate_second) {
    return false;
  }
  if (pairedDualOptionsWidget(candidate_first) != nullptr || pairedDualOptionsWidget(candidate_second) != nullptr) {
    return false;
  }
  if (candidate_first->parentWidget() == nullptr ||
      candidate_first->parentWidget() != candidate_second->parentWidget()) {
    return false;
  }

  QRadioButton* first = candidate_first;
  QRadioButton* second = candidate_second;
  QWidget* parent = first->parentWidget();
  if (!sortRadioPairByLayout(parent, first, second)) {
    return false;
  }
  if (first->text().isEmpty() || second->text().isEmpty()) {
    return false;
  }
  if (!radioPairHasExplicitExclusiveGroup(first, second)) {
    return false;
  }
  if (first->isChecked() == second->isChecked()) {
    return false;
  }

  auto* dual = new DualOptionsWidget(first->text(), second->text(), parent);
  dual->setToolTip(parent->toolTip());
  dual->setEnabled(first->isEnabled() && second->isEnabled());
  dual->setSelectedIndex(second->isChecked() ? 1 : 0);
  dual->setProperty(kDualOptionsRadio0Property, QVariant::fromValue<QObject*>(first));
  dual->setProperty(kDualOptionsRadio1Property, QVariant::fromValue<QObject*>(second));
  first->setProperty(kDualOptionsWidgetProperty, QVariant::fromValue<QObject*>(dual));
  first->setProperty(kDualOptionsIndexProperty, 0);
  first->setProperty(kDualOptionsDesiredVisibleProperty, !first->isHidden());
  second->setProperty(kDualOptionsWidgetProperty, QVariant::fromValue<QObject*>(dual));
  second->setProperty(kDualOptionsIndexProperty, 1);
  second->setProperty(kDualOptionsDesiredVisibleProperty, !second->isHidden());

  insertDualOptionsWidget(parent, first, second, dual);
  first->hide();
  second->hide();

  const QPointer<QRadioButton> first_ptr(first);
  const QPointer<QRadioButton> second_ptr(second);
  QObject::connect(dual, &DualOptionsWidget::selectionChanged, dual, [first_ptr, second_ptr](int index) {
    QRadioButton* selected = (index == 0) ? first_ptr.data() : second_ptr.data();
    if (selected != nullptr) {
      selected->setChecked(true);
    }
  });
  QObject::connect(first, &QRadioButton::toggled, dual, [first, dual](bool checked) {
    if (checked) {
      dual->setSelectedIndex(0);
    }
    first->hide();
  });
  QObject::connect(second, &QRadioButton::toggled, dual, [second, dual](bool checked) {
    if (checked) {
      dual->setSelectedIndex(1);
    }
    second->hide();
  });
  return true;
}

// Try to adapt the exclusive two-button group that `radio` belongs to. No-op
// when the radio is already adapted, has no group, the group is non-exclusive,
// or the group isn't exactly two buttons sharing `radio`'s parent. Cheap and
// idempotent — safe to call per radio whenever its data is applied, which is
// how a pair that only becomes adaptable AFTER plugin data selects an option
// gets converted without re-walking the whole widget tree on every data tick.
static void tryAdaptRadioGroup(QRadioButton* radio) {
  if (radio == nullptr || pairedDualOptionsWidget(radio) != nullptr) {
    return;
  }
  QButtonGroup* group = radio->group();
  QWidget* parent = radio->parentWidget();
  if (group == nullptr || !group->exclusive() || parent == nullptr) {
    return;
  }
  QList<QRadioButton*> group_radios;
  for (auto* button : group->buttons()) {
    if (auto* rb = qobject_cast<QRadioButton*>(button); rb != nullptr && rb->parentWidget() == parent) {
      group_radios.push_back(rb);
    }
  }
  if (group_radios.size() == 2) {
    (void)tryAdaptRadioPair(group_radios[0], group_radios[1]);
  }
}

// --- QCheckBox -> ToggleSwitch ------------------------------------------------

constexpr const char* kToggleSwitchProperty = "_pj_toggle_switch";                   // on the hidden QCheckBox
constexpr const char* kToggleCheckBoxProperty = "_pj_toggle_checkbox";               // on the ToggleSwitch
constexpr const char* kToggleDesiredVisibleProperty = "_pj_toggle_desired_visible";  // on the hidden QCheckBox

static PJ::ToggleSwitch* pairedToggleSwitch(const QWidget* widget) {
  QObject* obj = widget->property(kToggleSwitchProperty).value<QObject*>();
  return qobject_cast<PJ::ToggleSwitch*>(obj);
}

// True when `widget` lives inside one of the host composite widgets that manage
// their own internal child controls (CredentialsEditor's allow-insecure box,
// DateRangePicker's sub-widgets). Those internals are opaque — adapting a
// checkbox buried in them would corrupt the composite, so they are left alone.
static bool isInsideHostComposite(const QWidget* widget) {
  for (QWidget* p = widget->parentWidget(); p != nullptr; p = p->parentWidget()) {
    if (qobject_cast<CredentialsEditor*>(p) != nullptr || qobject_cast<DateRangePicker*>(p) != nullptr) {
      return true;
    }
  }
  return false;
}

// Push the checkbox's current state onto its ToggleSwitch (keeping it hidden).
static void syncToggleFromCheckBox(QCheckBox* checkbox) {
  auto* toggle = pairedToggleSwitch(checkbox);
  if (toggle == nullptr) {
    return;
  }
  checkbox->hide();
  toggle->setEnabled(checkbox->isEnabled());
  toggle->setVisible(checkbox->property(kToggleDesiredVisibleProperty).toBool());
  const QSignalBlocker blocker(toggle);
  toggle->setChecked(checkbox->isChecked(), /*animate=*/false);
}

// Swap `old_w` for `new_w` in `parent`'s layout, preserving position/span.
// QLayout::replaceWidget searches nested layouts recursively and returns the
// item that wrapped `old_w` (which we delete; `old_w` itself stays alive).
static bool replaceWidgetInLayout(QWidget* parent, QWidget* old_w, QWidget* new_w) {
  QLayout* layout = parent->layout();
  if (layout == nullptr) {
    return false;
  }
  QLayoutItem* old_item = layout->replaceWidget(old_w, new_w);
  if (old_item == nullptr) {
    return false;
  }
  delete old_item;
  return true;
}

// Replace a plain QCheckBox with a labelled ToggleSwitch (label on the LEFT,
// switch on the right). The checkbox is kept hidden+alive so plugin data and
// the connectWidgetSignals event callback keep flowing through it; the toggle
// just drives it. No-op for tristate / textless / composite-internal / already
// adapted checkboxes.
static bool tryAdaptCheckBox(QCheckBox* checkbox) {
  if (checkbox == nullptr || pairedToggleSwitch(checkbox) != nullptr) {
    return false;
  }
  if (checkbox->isTristate() || checkbox->text().isEmpty()) {
    return false;
  }
  QWidget* parent = checkbox->parentWidget();
  if (parent == nullptr || parent->layout() == nullptr || isInsideHostComposite(checkbox)) {
    return false;
  }

  auto* toggle = new ToggleSwitch(parent);
  toggle->setText(checkbox->text());
  toggle->setLabelSide(ToggleSwitch::LabelSide::Left);
  toggle->setToolTip(checkbox->toolTip());
  toggle->setEnabled(checkbox->isEnabled());
  toggle->setChecked(checkbox->isChecked(), /*animate=*/false);

  if (!replaceWidgetInLayout(parent, checkbox, toggle)) {
    delete toggle;
    return false;
  }
  checkbox->setProperty(kToggleSwitchProperty, QVariant::fromValue<QObject*>(toggle));
  checkbox->setProperty(kToggleDesiredVisibleProperty, !checkbox->isHidden());
  toggle->setProperty(kToggleCheckBoxProperty, QVariant::fromValue<QObject*>(checkbox));
  checkbox->hide();

  const QPointer<QCheckBox> cb_ptr(checkbox);
  QObject::connect(toggle, &ToggleSwitch::toggled, toggle, [cb_ptr](bool checked) {
    if (cb_ptr != nullptr) {
      cb_ptr->setChecked(checked);
    }
  });
  QObject::connect(checkbox, &QCheckBox::toggled, toggle, [checkbox, toggle](bool checked) {
    const QSignalBlocker blocker(toggle);
    toggle->setChecked(checked, /*animate=*/false);
    checkbox->hide();
  });
  return true;
}

}  // namespace

void adaptRadioButtonPairs(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // One pass over every radio; tryAdaptRadioGroup is idempotent, so visiting
  // both members of a pair just no-ops the second time.
  const QList<QRadioButton*> radios = root->findChildren<QRadioButton*>();
  for (QRadioButton* radio : radios) {
    tryAdaptRadioGroup(radio);
  }
}

void adaptCheckBoxes(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  const QList<QCheckBox*> checkboxes = root->findChildren<QCheckBox*>();
  for (QCheckBox* checkbox : checkboxes) {
    (void)tryAdaptCheckBox(checkbox);
  }
}

void adaptComboBoxes(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  // Unlike radios/checkboxes, PJ::ComboBox IS a QComboBox, so there is no swap:
  // we upgrade each plain QComboBox in place (gradient delegate + popup frame
  // fixes), which preserves its model, current index, and signal connections.
  // Skip ones already promoted to PJ::ComboBox, ones inside opaque host
  // composites, and ones already upgraded (marker keeps it idempotent).
  static constexpr const char* kComboStyledProperty = "_pj_combo_styled";
  const QList<QComboBox*> combos = root->findChildren<QComboBox*>();
  for (QComboBox* combo : combos) {
    if (qobject_cast<ComboBox*>(combo) != nullptr || combo->property(kComboStyledProperty).toBool() ||
        isInsideHostComposite(combo)) {
      continue;
    }
    applyComboBoxStyling(combo);
    combo->setProperty(kComboStyledProperty, true);
  }
}

void adaptScrollAreas(QWidget* root) {
  if (root == nullptr) {
    return;
  }
  static constexpr bool kDefaultAutoHide = true;
  static constexpr int kDefaultFadeMs = 150;

  const QList<QAbstractScrollArea*> areas = root->findChildren<QAbstractScrollArea*>();
  for (QAbstractScrollArea* area : areas) {
    if (area->property("pjScrollbarAttached").toBool() || isInsideHostComposite(area)) {
      continue;
    }
    // FIX 7: skip the internal scroll area of a combo-box container — adapting
    // it would add pill overlays to the combo's own view and/or its popup list.
    if (qobject_cast<QComboBox*>(area->parentWidget()) != nullptr) {
      continue;
    }
    // FIX 7: skip a QAbstractItemView whose top-level window is a popup (e.g.
    // the QListView Qt opens for a combo-box drop-down in a transient popup
    // window — it is closed on selection and should never receive overlays).
    if (qobject_cast<QAbstractItemView*>(area) != nullptr && (area->window()->windowFlags() & Qt::Popup) == Qt::Popup) {
      continue;
    }

    // Respect a deliberately-pinned scrollbar: a plugin that set an axis to
    // AlwaysOn wants a persistent, draggable native bar (e.g. a log/console
    // view), which a hover-only pill would silently replace. Skip that axis and
    // leave its native bar untouched. AsNeeded (the default) and AlwaysOff are
    // both compatible with the pill (the pill paints over a hidden gutter).
    const bool adapt_h = area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOn;
    const bool adapt_v = area->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOn;
    if (!adapt_h && !adapt_v) {
      continue;  // both axes pinned by the plugin; nothing to adapt
    }

    const bool auto_hide = area->property("pjScrollbarAutoHide").isValid()
                               ? area->property("pjScrollbarAutoHide").toBool()
                               : kDefaultAutoHide;
    const int fade_ms =
        area->property("pjScrollbarFadeMs").isValid() ? area->property("pjScrollbarFadeMs").toInt() : kDefaultFadeMs;

    const auto attach_pill = [&](Qt::Orientation orientation) {
      auto* pill = new PJ::Scrollbar(orientation, area);
      pill->attach(area);
      pill->setAutoHide(auto_hide);
      pill->setFadeDurationMs(fade_ms);
    };
    if (adapt_h) {
      attach_pill(Qt::Horizontal);
    }
    if (adapt_v) {
      attach_pill(Qt::Vertical);
    }

    area->setProperty("pjScrollbarAttached", true);
  }
}

void adaptStyledWidgets(QWidget* root) {
  adaptRadioButtonPairs(root);
  adaptCheckBoxes(root);
  adaptComboBoxes(root);
  adaptScrollAreas(root);
}

void tryAdaptStyledWidget(QWidget* w) {
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    tryAdaptRadioGroup(rb);
    return;
  }
  if (auto* cb = qobject_cast<QCheckBox*>(w)) {
    (void)tryAdaptCheckBox(cb);
    return;
  }
}

void syncStyledWidget(QWidget* w) {
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    syncDualOptionsFromRadioPair(rb);
    return;
  }
  if (auto* cb = qobject_cast<QCheckBox*>(w)) {
    syncToggleFromCheckBox(cb);
    return;
  }
}

bool redirectAdaptedVisibility(QWidget* w, bool visible) {
  if (pairedDualOptionsWidget(w) != nullptr) {
    w->setProperty(kDualOptionsDesiredVisibleProperty, visible);
    return true;
  }
  if (pairedToggleSwitch(w) != nullptr) {
    w->setProperty(kToggleDesiredVisibleProperty, visible);
    return true;
  }
  return false;
}

}  // namespace PJ
