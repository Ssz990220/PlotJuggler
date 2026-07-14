// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SectionHeaderBand.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <algorithm>

#include "pj_widgets/ComboBox.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/Search.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

SectionHeaderBand::SectionHeaderBand(const QString& title, QWidget* parent) : QWidget(parent) {
  // QWidget subclasses don't paint stylesheet backgrounds unless told to
  // (plain QWidgets from .ui files get this implicitly).
  setAttribute(Qt::WA_StyledBackground, true);
  // Start at the first-launch canonical height; the host's chromeMetricsChanged
  // broadcast (routed to onChromeMetricsChanged) keeps it in step thereafter.
  setFixedHeight(ChromeMetrics{}.bandHeight());

  layout_ = new QHBoxLayout(this);
  layout_->setContentsMargins(
      theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
      theme::space(theme::Space::None));
  layout_->setSpacing(theme::space(theme::Space::None));

  label_ = new QLabel(title, this);
  // Leading inset via the label's indent, not layout margins — hosts that
  // re-apply chrome metrics overwrite the layout margins at runtime.
  label_->setIndent(theme::space(theme::Space::Tight));
  layout_->addWidget(label_);
  layout_->addStretch(1);
  // Remember the stretch so an expanding filter/combo can replace it later —
  // by then it is no longer the last item (trailing controls may exist).
  stretch_ = layout_->itemAt(layout_->count() - 1);
}

void SectionHeaderBand::takeStretch() {
  if (stretch_ == nullptr) {
    return;
  }
  for (int i = 0; i < layout_->count(); ++i) {
    if (layout_->itemAt(i) == stretch_) {
      delete layout_->takeAt(i);
      break;
    }
  }
  stretch_ = nullptr;
}

void SectionHeaderBand::ensureFilter() {
  if (filter_search_ != nullptr) {
    return;
  }
  // The trailing stretch keeps a bare title left-aligned; the expanding filter
  // field takes over that role once the filter exists, so drop it first.
  takeStretch();

  // The canonical filter control: glyph + flat field sharing one background, the
  // kBanner tone matching the band. It sizes to the band via onChromeMetricsChanged.
  // Inserted right after the title so later-created trailing controls always end
  // up on its right, whatever order the .ui set the properties in.
  layout_->insertSpacing(1, theme::space(theme::Space::Tight));
  filter_search_ = new Search(this);
  filter_search_->setFieldObjectName(filter_field_name_);
  filter_search_->setChromeMetrics(current_metrics_);
  filter_search_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout_->insertWidget(2, filter_search_, 1, Qt::AlignVCenter);
}

QLineEdit* SectionHeaderBand::filterEdit() const {
  return filter_search_ != nullptr ? filter_search_->lineEdit() : nullptr;
}

void SectionHeaderBand::setFilterPlaceholder(const QString& placeholder) {
  if (placeholder.isEmpty()) {
    return;
  }
  ensureFilter();
  filter_search_->setPlaceholder(placeholder);
}

QString SectionHeaderBand::filterPlaceholder() const {
  return filter_search_ != nullptr ? filter_search_->placeholder() : QString();
}

void SectionHeaderBand::setFilterFieldName(const QString& name) {
  filter_field_name_ = name;
  if (filter_search_ != nullptr) {
    filter_search_->setFieldObjectName(name);
  }
}

QString SectionHeaderBand::filterFieldName() const {
  return filter_field_name_;
}

void SectionHeaderBand::ensureTrailingCombo() {
  if (trailing_combo_ != nullptr) {
    return;
  }
  // A PJ::ComboBox gives the gradient popup without relying on the host's
  // combo adapter.
  trailing_combo_ = new ComboBox(this);
  trailing_combo_->setObjectName(trailing_combo_name_);
  trailing_combo_->setEditable(combo_editable_);
  trailing_combo_->setMaximumHeight(height());
  if (combo_expanding_) {
    // "Label + input" banner row: the combo takes over the stretch and fills
    // the band right after the title (e.g. a "Server:" URL bar).
    takeStretch();
    layout_->insertSpacing(1, theme::space(theme::Space::Tight));
    trailing_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->insertWidget(2, trailing_combo_, 1, Qt::AlignVCenter);
  } else {
    // The band's `label_ + stretch(1)` already right-justifies anything
    // appended; the dropdown docks to the right edge (with a little inset).
    layout_->addWidget(trailing_combo_, 0, Qt::AlignVCenter);
    layout_->addSpacing(theme::space(theme::Space::Comfortable));
  }
}

void SectionHeaderBand::setTrailingComboName(const QString& name) {
  trailing_combo_name_ = name;
  if (name.isEmpty()) {
    return;
  }
  ensureTrailingCombo();
  trailing_combo_->setObjectName(name);
}

QString SectionHeaderBand::trailingComboName() const {
  return trailing_combo_name_;
}

void SectionHeaderBand::setComboExpanding(bool expanding) {
  if (combo_expanding_ == expanding) {
    return;
  }
  combo_expanding_ = expanding;
  if (trailing_combo_ != nullptr && expanding) {
    // Created docked first (property order): move it into the expanding slot.
    layout_->removeWidget(trailing_combo_);
    takeStretch();
    layout_->insertSpacing(1, theme::space(theme::Space::Tight));
    trailing_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->insertWidget(2, trailing_combo_, 1, Qt::AlignVCenter);
  }
}

void SectionHeaderBand::setComboEditable(bool editable) {
  combo_editable_ = editable;
  if (trailing_combo_ != nullptr) {
    trailing_combo_->setEditable(editable);
  }
}

void SectionHeaderBand::ensureTrailingButton() {
  if (trailing_button_ != nullptr) {
    return;
  }
  // A flat, borderless icon button so it reads as a band affordance (like the
  // filter's search glyph) rather than a chunky QPushButton. It stays a real
  // QPushButton so the dialog host routes its click with no special-casing.
  trailing_button_ = new QPushButton(this);
  trailing_button_->setFlat(true);
  trailing_button_->setCursor(Qt::PointingHandCursor);
  trailing_button_->setFocusPolicy(Qt::NoFocus);
  trailing_button_->setStyleSheet(
      QStringLiteral("QPushButton { border: none; padding: %1px; }").arg(theme::space(theme::Space::None)));
  const int icon_px = 18;
  trailing_button_->setIconSize(QSize(icon_px, icon_px));
  trailing_button_->setFixedSize(icon_px + 8, icon_px + 8);
  layout_->addWidget(trailing_button_, 0, Qt::AlignVCenter);
  layout_->addSpacing(theme::space(theme::Space::Snug));
}

void SectionHeaderBand::setTrailingButtonName(const QString& name) {
  trailing_button_name_ = name;
  ensureTrailingButton();
  trailing_button_->setObjectName(name);
}

QString SectionHeaderBand::trailingButtonName() const {
  return trailing_button_name_;
}

void SectionHeaderBand::setTrailingButtonIcon(const QString& svgPath) {
  trailing_button_icon_ = svgPath;
  if (svgPath.isEmpty()) {
    return;
  }
  ensureTrailingButton();
  trailing_button_->setIcon(loadSvg(svgPath, currentTheme()));
}

QString SectionHeaderBand::trailingButtonIcon() const {
  return trailing_button_icon_;
}

int SectionHeaderBand::trailingInsertIndex(bool before_buttons) const {
  int idx = layout_->count();
  if (trailing_combo_ != nullptr && !combo_expanding_) {
    idx = std::min(idx, layout_->indexOf(trailing_combo_));
  }
  if (before_buttons) {
    if (!trailing_buttons_.isEmpty()) {
      idx = std::min(idx, layout_->indexOf(trailing_buttons_.first()));
    }
    if (trailing_button_ != nullptr) {
      idx = std::min(idx, layout_->indexOf(trailing_button_));
    }
  }
  return idx;
}

void SectionHeaderBand::applyButtonMetrics(QPushButton* button) const {
  // Same recipe as the toolbox banner's close button: a bandHeight box holding
  // an icon_size icon, so band affordances read at one size everywhere.
  button->setFixedSize(current_metrics_.bandHeight(), current_metrics_.bandHeight());
  button->setIconSize(QSize(current_metrics_.icon_size, current_metrics_.icon_size));
}

void SectionHeaderBand::ensureTrailingToggle() {
  if (trailing_toggle_ != nullptr) {
    return;
  }
  trailing_toggle_ = new QPushButton(this);
  trailing_toggle_->setCheckable(true);
  trailing_toggle_->setFlat(true);
  trailing_toggle_->setCursor(Qt::PointingHandCursor);
  trailing_toggle_->setFocusPolicy(Qt::NoFocus);
  applyButtonMetrics(trailing_toggle_);
  layout_->insertWidget(trailingInsertIndex(/*before_buttons=*/true), trailing_toggle_, 0, Qt::AlignVCenter);
}

void SectionHeaderBand::ensureTrailingButtons(int count) {
  while (trailing_buttons_.size() < count) {
    auto* button = new QPushButton(this);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    applyButtonMetrics(button);
    layout_->insertWidget(trailingInsertIndex(/*before_buttons=*/false), button, 0, Qt::AlignVCenter);
    trailing_buttons_.append(button);
  }
}

void SectionHeaderBand::setTrailingButtonNames(const QStringList& names) {
  trailing_button_names_ = names;
  ensureTrailingButtons(static_cast<int>(names.size()));
  for (int i = 0; i < names.size(); ++i) {
    trailing_buttons_[i]->setObjectName(names[i]);
    if (i < trailing_button_tooltips_.size()) {
      trailing_buttons_[i]->setToolTip(trailing_button_tooltips_[i]);
    }
  }
}

QStringList SectionHeaderBand::trailingButtonNames() const {
  return trailing_button_names_;
}

void SectionHeaderBand::setTrailingButtonToolTips(const QStringList& tooltips) {
  trailing_button_tooltips_ = tooltips;
  const int n = static_cast<int>(std::min(tooltips.size(), trailing_buttons_.size()));
  for (int i = 0; i < n; ++i) {
    trailing_buttons_[i]->setToolTip(tooltips[i]);
  }
}

QStringList SectionHeaderBand::trailingButtonToolTips() const {
  return trailing_button_tooltips_;
}

void SectionHeaderBand::setTrailingToggleName(const QString& name) {
  trailing_toggle_name_ = name;
  ensureTrailingToggle();
  trailing_toggle_->setObjectName(name);
}

QString SectionHeaderBand::trailingToggleName() const {
  return trailing_toggle_name_;
}

void SectionHeaderBand::setTrailingToggleText(const QString& text) {
  trailing_toggle_text_ = text;
  ensureTrailingToggle();
  trailing_toggle_->setText(text);
}

QString SectionHeaderBand::trailingToggleText() const {
  return trailing_toggle_text_;
}

void SectionHeaderBand::setTrailingToggleToolTip(const QString& tooltip) {
  trailing_toggle_tooltip_ = tooltip;
  ensureTrailingToggle();
  trailing_toggle_->setToolTip(tooltip);
}

QString SectionHeaderBand::trailingToggleToolTip() const {
  return trailing_toggle_tooltip_;
}

void SectionHeaderBand::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  current_metrics_ = metrics;
  setFixedHeight(metrics.bandHeight());
  if (trailing_combo_ != nullptr) {
    trailing_combo_->setMaximumHeight(metrics.bandHeight());
  }
  if (filter_search_ != nullptr) {
    filter_search_->setChromeMetrics(metrics);
  }
  if (trailing_toggle_ != nullptr) {
    applyButtonMetrics(trailing_toggle_);
  }
  for (auto* button : trailing_buttons_) {
    applyButtonMetrics(button);
  }
}

void SectionHeaderBand::setText(const QString& title) {
  label_->setText(title);
}

QString SectionHeaderBand::text() const {
  return label_->text();
}

void SectionHeaderBand::setTitleObjectName(const QString& name) {
  label_->setObjectName(name);
}

QString SectionHeaderBand::titleObjectName() const {
  return label_->objectName();
}

}  // namespace PJ
