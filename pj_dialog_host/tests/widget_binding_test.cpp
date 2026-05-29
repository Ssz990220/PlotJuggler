// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the dialog building blocks added for the 0.4.0 dialog contract:
// the PjUiLoader custom-widget vocabulary (RangeSlider, DateRangePicker), the
// RangeSlider data binding (bounds/values + duration labels), and the generic
// field-validity indicator (setFieldValid).

#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QLineEdit>
#include <QWidget>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

QApplication* qapp() {
  static int argc = 0;
  static QApplication app(argc, nullptr);
  return &app;
}

struct Event {
  std::string name;
  std::string json;
};

// Wire connectWidgetSignals to a recorder so tests can assert what the plugin
// would have received.
std::vector<Event>* recorder() {
  static std::vector<Event> events;
  return &events;
}

// PjUiLoader resolves the host's custom widget classes by name; plain Qt classes
// fall through to the base QUiLoader.
TEST(PjUiLoader, RegistersBuildingBlocks) {
  qapp();
  const QByteArray ui = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>Root</class>
 <widget class="QWidget" name="Root">
  <layout class="QVBoxLayout">
   <item><widget class="RangeSlider" name="rangeSlider"/></item>
   <item><widget class="DateRangePicker" name="datePicker"/></item>
   <item><widget class="QLineEdit" name="plainEdit"/></item>
  </layout>
 </widget>
</ui>)";
  QByteArray data(ui);
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  PJ::PjUiLoader loader;
  QWidget* root = loader.load(&buffer);
  ASSERT_NE(root, nullptr);
  EXPECT_NE(root->findChild<PJ::RangeSlider*>("rangeSlider"), nullptr);
  EXPECT_NE(root->findChild<PJ::DateRangePicker*>("datePicker"), nullptr);
  EXPECT_NE(root->findChild<QLineEdit*>("plainEdit"), nullptr);
  delete root;
}

// Bounds + handle values are applied, and a time span turns on the floating
// duration labels.
TEST(WidgetBindingRangeSlider, AppliesBoundsValuesAndTimeSpan) {
  qapp();
  QWidget root;
  auto* slider = new PJ::RangeSlider(Qt::Horizontal, PJ::RangeSlider::DoubleHandles, &root);
  slider->setObjectName("rangeSlider");

  PJ::WidgetData wd;
  wd.setRangeSliderBounds("rangeSlider", 0, 1000);
  wd.setRangeSliderValues("rangeSlider", 200, 800);
  wd.setRangeSliderTimeSpan("rangeSlider", 0, 1'000'000'000'000LL);
  PJ::WidgetDataView view(wd.toJson());
  PJ::applyWidgetData(&root, view);

  EXPECT_EQ(slider->GetMaximun(), 1000);
  EXPECT_EQ(slider->GetLowerValue(), 200);
  EXPECT_EQ(slider->GetUpperValue(), 800);
  EXPECT_TRUE(slider->floatingLabelsVisible());
}

// The plugin owns the rule and pushes {valid, tooltip}; the host renders the
// tooltip plus a red border when invalid, and clears it when valid.
TEST(WidgetBindingFieldValidity, RendersTooltipAndBorder) {
  qapp();
  QWidget root;
  auto* edit = new QLineEdit(&root);
  edit->setObjectName("apiKey");

  PJ::WidgetData bad;
  bad.setFieldValid("apiKey", false, "invalid key");
  PJ::applyWidgetData(&root, PJ::WidgetDataView(bad.toJson()));
  EXPECT_EQ(edit->toolTip().toStdString(), "invalid key");
  EXPECT_TRUE(edit->styleSheet().contains("border")) << "invalid field should show a border cue";

  PJ::WidgetData good;
  good.setFieldValid("apiKey", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(good.toJson()));
  EXPECT_TRUE(edit->styleSheet().isEmpty()) << "valid field should clear the border cue";
}

// --- Editable QComboBox handling (generic; ported from gor/mosaico) ----------

TEST(WidgetBindingCombo, EditableComboForwardsTypedTextAsTextChanged) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("comboUri");
  combo->setEditable(true);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  combo->setEditText("grpc+tls://my.server:6726");

  // At least one event must carry the typed text under the "text" key so the
  // typed dispatcher routes it to onTextChanged.
  bool saw_text = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "comboUri") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("text") && j["text"] == "grpc+tls://my.server:6726") {
      saw_text = true;
    }
  }
  EXPECT_TRUE(saw_text) << "editable combo edit-text must emit a text_changed event";
}

TEST(WidgetBindingCombo, EditableComboReflectsPluginText) {
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("comboUri");
  combo->setEditable(true);

  PJ::WidgetData wd;
  wd.setText("comboUri", "host.example:9999");
  PJ::WidgetDataView view(wd.toJson());
  PJ::applyWidgetData(&root, view);

  EXPECT_EQ(combo->currentText().toStdString(), "host.example:9999");
}

TEST(WidgetBindingCombo, NonEditableComboEmitsIndexNotText) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("mode");
  combo->addItems({"alpha", "beta", "gamma"});

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  combo->setCurrentIndex(2);

  bool saw_index = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "mode") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("current_index")) {
      saw_index = true;
      EXPECT_FALSE(j.contains("text")) << "non-editable combo must not masquerade as text";
    }
  }
  EXPECT_TRUE(saw_index) << "non-editable combo selection must emit an index event";
}

TEST(WidgetBindingCombo, IdenticalItemsPreserveTypedText) {
  // Re-applying the SAME item set must not clear an editable combo's line edit
  // (which would bounce the caret to the end while the user is mid-typing).
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("c");
  combo->setEditable(true);
  combo->addItems({"alpha", "beta"});
  combo->setCurrentText("user.typed.host:1");

  PJ::WidgetData wd;
  wd.setItems("c", {"alpha", "beta"});  // same items, no text field
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(combo->currentText().toStdString(), "user.typed.host:1");
}

TEST(WidgetBindingCombo, ChangedItemsRebuild) {
  qapp();
  QWidget root;
  auto* combo = new QComboBox(&root);
  combo->setObjectName("c");
  combo->setEditable(true);
  combo->addItems({"alpha"});

  PJ::WidgetData wd;
  wd.setItems("c", {"alpha", "beta", "gamma"});
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(combo->count(), 3);
}

}  // namespace
