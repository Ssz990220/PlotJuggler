// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/SectionHeaderBand.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

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
}

void SectionHeaderBand::ensureFilter() {
  if (filter_search_ != nullptr) {
    return;
  }
  // The trailing stretch keeps a bare title left-aligned; the expanding filter
  // field takes over that role once the filter exists, so drop it first.
  if (QLayoutItem* stretch = layout_->takeAt(layout_->count() - 1); stretch != nullptr) {
    delete stretch;
  }

  // The canonical filter control: glyph + flat field sharing one background, the
  // kBanner tone matching the band. It sizes to the band via onChromeMetricsChanged.
  layout_->addSpacing(theme::space(theme::Space::Tight));
  filter_search_ = new Search(this);
  filter_search_->setFieldObjectName(filter_field_name_);
  filter_search_->setChromeMetrics(current_metrics_);
  filter_search_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout_->addWidget(filter_search_, 1, Qt::AlignVCenter);
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
  // The band's `label_ + stretch(1)` already right-justifies anything appended;
  // add the combo after the stretch so the title stays left and the dropdown
  // docks to the right edge (with a little inset). A PJ::ComboBox gives the
  // gradient popup without relying on the host's combo adapter.
  trailing_combo_ = new ComboBox(this);
  trailing_combo_->setObjectName(trailing_combo_name_);
  trailing_combo_->setMaximumHeight(height());
  layout_->addWidget(trailing_combo_, 0, Qt::AlignVCenter);
  layout_->addSpacing(theme::space(theme::Space::Comfortable));
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

void SectionHeaderBand::onChromeMetricsChanged(const ChromeMetrics& metrics) {
  current_metrics_ = metrics;
  setFixedHeight(metrics.bandHeight());
  if (trailing_combo_ != nullptr) {
    trailing_combo_->setMaximumHeight(metrics.bandHeight());
  }
  if (filter_search_ != nullptr) {
    filter_search_->setChromeMetrics(metrics);
  }
}

void SectionHeaderBand::setText(const QString& title) {
  label_->setText(title);
}

QString SectionHeaderBand::text() const {
  return label_->text();
}

}  // namespace PJ
