// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/ParameterForm.h"

#include <QDoubleValidator>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QString>
#include <algorithm>
#include <limits>

#include "pj_widgets/ComboBox.h"
#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/ToggleSwitch.h"

namespace PJ {

using scripting::ParamSpec;
using scripting::ParamType;

ParameterForm::ParameterForm(QWidget* parent) : QWidget(parent) {
  form_ = new QFormLayout(this);
  form_->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  form_->setLabelAlignment(Qt::AlignLeft);
}

void ParameterForm::clear() {
  rows_.clear();
  // QFormLayout owns the row widgets; clearing the layout deletes them.
  while (form_->rowCount() > 0) {
    form_->removeRow(0);
  }
}

void ParameterForm::setSchema(const std::vector<ParamSpec>& schema) {
  clear();
  silent_ = true;
  for (const ParamSpec& spec : schema) {
    auto* label = new QLabel(QString::fromStdString(spec.label.empty() ? spec.name : spec.label), this);
    if (!spec.tooltip.empty()) {
      label->setToolTip(QString::fromStdString(spec.tooltip));
    }
    QWidget* editor = nullptr;

    switch (spec.type) {
      case ParamType::kInteger: {
        auto* w = new QSpinBox(this);
        // QSpinBox is 32-bit; a schema min/max documented as int64 must be CLAMPED to the int
        // range, not truncated by a blind static_cast<int> (which would wrap e.g. 5e9 to garbage).
        const long long lo = spec.min ? static_cast<long long>(*spec.min) : -1'000'000'000LL;
        const long long hi = spec.max ? static_cast<long long>(*spec.max) : 1'000'000'000LL;
        constexpr long long kIntMin = std::numeric_limits<int>::min();
        constexpr long long kIntMax = std::numeric_limits<int>::max();
        w->setRange(
            static_cast<int>(std::clamp(lo, kIntMin, kIntMax)), static_cast<int>(std::clamp(hi, kIntMin, kIntMax)));
        if (spec.step) {
          w->setSingleStep(static_cast<int>(*spec.step));
        }
        w->setValue(spec.default_value.is_number() ? spec.default_value.get<int>() : 0);
        connect(w, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { onEditorChanged(); });
        editor = w;
        break;
      }
      case ParamType::kNumber: {
        auto* w = new QLineEdit(this);
        auto* validator = new QDoubleValidator(w);  // full double precision (handles 1e-9)
        validator->setNotation(QDoubleValidator::ScientificNotation);
        if (spec.min) {
          validator->setBottom(*spec.min);
        }
        if (spec.max) {
          validator->setTop(*spec.max);
        }
        if (spec.decimals) {
          validator->setDecimals(*spec.decimals);
        }
        w->setValidator(validator);
        const double d = spec.default_value.is_number() ? spec.default_value.get<double>() : 0.0;
        w->setText(QString::number(d, 'g', 17));
        connect(w, &QLineEdit::textChanged, this, [this](const QString&) { onEditorChanged(); });
        editor = w;
        break;
      }
      case ParamType::kBoolean: {
        auto* w = new ToggleSwitch(this);
        w->setChecked(spec.default_value.is_boolean() && spec.default_value.get<bool>(), false);
        connect(w, &ToggleSwitch::toggled, this, [this](bool) { onEditorChanged(); });
        editor = w;
        break;
      }
      case ParamType::kEnum: {
        auto* w = new ComboBox(this);
        for (const scripting::EnumValue& ev : spec.values) {
          w->addItem(QString::fromStdString(ev.label));
        }
        // Default = the index whose value matches default_value (else 0).
        int idx = 0;
        for (std::size_t i = 0; i < spec.values.size(); ++i) {
          if (spec.values[i].value == spec.default_value) {
            idx = static_cast<int>(i);
            break;
          }
        }
        w->setCurrentIndex(idx);
        connect(w, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { onEditorChanged(); });
        editor = w;
        break;
      }
      case ParamType::kString: {
        auto* w = new QLineEdit(this);
        if (spec.default_value.is_string()) {
          w->setText(QString::fromStdString(spec.default_value.get<std::string>()));
        }
        connect(w, &QLineEdit::textChanged, this, [this](const QString&) { onEditorChanged(); });
        editor = w;
        break;
      }
      case ParamType::kText: {
        auto* w = new QPlainTextEdit(this);
        if (spec.default_value.is_string()) {
          w->setPlainText(QString::fromStdString(spec.default_value.get<std::string>()));
        }
        connect(w, &QPlainTextEdit::textChanged, this, [this]() { onEditorChanged(); });
        editor = w;
        break;
      }
    }
    if (editor) {
      editor->setObjectName(QString::fromStdString(spec.name));  // for layout selectors / tests
      if (!spec.tooltip.empty()) {
        editor->setToolTip(QString::fromStdString(spec.tooltip));
      }
    }
    form_->addRow(label, editor);
    rows_.push_back(Row{spec, editor, label});
  }
  silent_ = false;
  updateConditionalVisibility();
}

void ParameterForm::onEditorChanged() {
  if (silent_) {
    return;
  }
  updateConditionalVisibility();
  emit changed();
}

nlohmann::json ParameterForm::values() const {
  nlohmann::json out = nlohmann::json::object();
  for (const Row& r : rows_) {
    const std::string& key = r.spec.name;
    switch (r.spec.type) {
      case ParamType::kInteger:
        out[key] = qobject_cast<QSpinBox*>(r.editor)->value();
        break;
      case ParamType::kNumber: {
        bool ok = false;
        const double d = qobject_cast<QLineEdit*>(r.editor)->text().toDouble(&ok);
        // Empty/intermediate text must not silently become 0 (e.g. binary_tol
        // 1e-9 → 0): fall back to the declared default.
        out[key] = ok ? d : (r.spec.default_value.is_number() ? r.spec.default_value.get<double>() : 0.0);
        break;
      }
      case ParamType::kBoolean:
        out[key] = qobject_cast<ToggleSwitch*>(r.editor)->isChecked();
        break;
      case ParamType::kEnum: {
        auto* cb = qobject_cast<ComboBox*>(r.editor);
        const int idx = cb->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(r.spec.values.size())) {
          out[key] = r.spec.values[idx].value;
        } else if (!r.spec.values.empty()) {
          out[key] = r.spec.values.front().value;  // out-of-range selection → declared first option
        } else if (!r.spec.default_value.is_null()) {
          out[key] = r.spec.default_value;
        } else {
          out[key] = nullptr;  // never omit the key: a create() relying on it must see *something*
        }
        break;
      }
      case ParamType::kString:
        out[key] = qobject_cast<QLineEdit*>(r.editor)->text().toStdString();
        break;
      case ParamType::kText:
        out[key] = qobject_cast<QPlainTextEdit*>(r.editor)->toPlainText().toStdString();
        break;
    }
  }
  return out;
}

void ParameterForm::setValues(const nlohmann::json& values) {
  silent_ = true;
  for (Row& r : rows_) {
    if (!values.contains(r.spec.name)) {
      continue;
    }
    const nlohmann::json& v = values.at(r.spec.name);
    switch (r.spec.type) {
      case ParamType::kInteger:
        if (v.is_number()) {
          qobject_cast<QSpinBox*>(r.editor)->setValue(v.get<int>());
        }
        break;
      case ParamType::kNumber:
        if (v.is_number()) {
          qobject_cast<QLineEdit*>(r.editor)->setText(QString::number(v.get<double>(), 'g', 17));
        }
        break;
      case ParamType::kBoolean:
        if (v.is_boolean()) {
          qobject_cast<ToggleSwitch*>(r.editor)->setChecked(v.get<bool>(), false);
        }
        break;
      case ParamType::kEnum: {
        auto* cb = qobject_cast<ComboBox*>(r.editor);
        for (std::size_t i = 0; i < r.spec.values.size(); ++i) {
          if (r.spec.values[i].value == v) {
            cb->setCurrentIndex(static_cast<int>(i));
            break;
          }
        }
        break;
      }
      case ParamType::kString:
        if (v.is_string()) {
          qobject_cast<QLineEdit*>(r.editor)->setText(QString::fromStdString(v.get<std::string>()));
        }
        break;
      case ParamType::kText:
        if (v.is_string()) {
          qobject_cast<QPlainTextEdit*>(r.editor)->setPlainText(QString::fromStdString(v.get<std::string>()));
        }
        break;
    }
  }
  silent_ = false;
  updateConditionalVisibility();
}

void ParameterForm::updateConditionalVisibility() {
  const nlohmann::json current = values();
  for (Row& r : rows_) {
    if (!r.spec.visible_when_param) {
      continue;
    }
    bool visible = false;
    if (auto it = current.find(*r.spec.visible_when_param); it != current.end()) {
      visible = (*it == r.spec.visible_when_equals);
    }
    if (r.editor) {
      r.editor->setVisible(visible);
    }
    if (r.label) {
      r.label->setVisible(visible);
    }
  }
}

}  // namespace PJ
