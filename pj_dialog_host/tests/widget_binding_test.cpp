// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the dialog building blocks added for the 0.4.0 dialog contract:
// the PjUiLoader custom-widget vocabulary (RangeSlider, DateRangePicker), the
// RangeSlider data binding (bounds/values + duration labels), and the generic
// field-validity indicator (setFieldValid).

#include <pj_widgets/ComboBox.h>
#include <pj_widgets/ComboBoxGradientDelegate.h>
#include <pj_widgets/CredentialsEditor.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/DualOptionsWidget.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/Scrollbar.h>
#include <pj_widgets/ToggleSwitch.h>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpacerItem>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <pj_plugins/sdk/widget_data.hpp>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

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
   <item><widget class="CredentialsEditor" name="certContents"/></item>
   <item><widget class="PJ::ComboBox" name="pjCombo"/></item>
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
  EXPECT_NE(root->findChild<PJ::CredentialsEditor*>("certContents"), nullptr);
  // The canonical dropdown must come back as the real PJ::ComboBox (gradient
  // popup), not the plain QComboBox the base loader would create.
  EXPECT_NE(root->findChild<PJ::ComboBox*>("pjCombo"), nullptr);
  // The cert dialog addresses CredentialsEditor's inner inputs by name; they
  // must be reachable for the plugin's setText("certPath"/...) to land.
  EXPECT_NE(root->findChild<QLineEdit*>("certPath"), nullptr);
  EXPECT_NE(root->findChild<QLineEdit*>("apiKey"), nullptr);
  EXPECT_NE(root->findChild<QLineEdit*>("plainEdit"), nullptr);
  delete root;
}

// Bounds + handle values are applied, and a time span turns on the floating
// duration labels.
TEST(WidgetBindingRangeSlider, AppliesBoundsValuesAndTimeSpan) {
  qapp();
  QWidget root;
  auto* slider = new PJ::RangeSlider(Qt::Horizontal, PJ::RangeSlider::kDoubleHandles, &root);
  slider->setObjectName("rangeSlider");

  PJ::WidgetData wd;
  wd.setRangeSliderBounds("rangeSlider", 0, 1000);
  wd.setRangeSliderValues("rangeSlider", 200, 800);
  wd.setRangeSliderTimeSpan("rangeSlider", 0, 1'000'000'000'000LL);
  PJ::WidgetDataView view(wd.toJson());
  PJ::applyWidgetData(&root, view);

  EXPECT_EQ(slider->getMaximun(), 1000);
  EXPECT_EQ(slider->getLowerValue(), 200);
  EXPECT_EQ(slider->getUpperValue(), 800);
  EXPECT_TRUE(slider->floatingLabelsVisible());
}

// The plugin owns the rule and pushes {valid, tooltip}; the host renders the
// tooltip plus a red border when invalid, and clears it when valid.
TEST(WidgetBindingFieldValidity, RendersTooltipAndBackground) {
  qapp();
  QWidget root;
  auto* edit = new QLineEdit(&root);
  edit->setObjectName("apiKey");

  PJ::WidgetData bad;
  bad.setFieldValid("apiKey", false, "invalid key");
  PJ::applyWidgetData(&root, PJ::WidgetDataView(bad.toJson()));
  EXPECT_EQ(edit->toolTip().toStdString(), "invalid key");
  // PJ3 parity: invalid fields get a light-red background, not a border.
  EXPECT_TRUE(edit->styleSheet().contains("background-color")) << "invalid field should show a background cue";

  PJ::WidgetData good;
  good.setFieldValid("apiKey", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(good.toJson()));
  EXPECT_TRUE(edit->styleSheet().isEmpty()) << "valid field should clear the cue";
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

TEST(WidgetBindingRadioGroupAdapter, ConvertsSafePairAndPreservesRadioEvents) {
  qapp();
  recorder()->clear();

  QWidget root;
  auto* root_layout = new QVBoxLayout(&root);
  auto* row = new QWidget(&root);
  auto* row_layout = new QHBoxLayout(row);
  auto* frame = new QRadioButton(u"Frame"_s, row);
  frame->setObjectName("frameMode");
  frame->setChecked(true);
  auto* arrow = new QRadioButton(u"Arrow"_s, row);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(row);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);
  root_layout->addWidget(row);

  PJ::adaptRadioGroups(&root);

  auto* dual = row->findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 0);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  dual->setSelectedIndex(1);

  EXPECT_TRUE(arrow->isChecked());
  EXPECT_FALSE(frame->isChecked());
  bool saw_arrow_checked = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "arrowMode") {
      continue;
    }
    const auto j = nlohmann::json::parse(ev.json, nullptr, false);
    saw_arrow_checked = !j.is_discarded() && j.value("checked", false);
  }
  EXPECT_TRUE(saw_arrow_checked) << "the hidden original radio button must still drive plugin onToggled callbacks";
}

TEST(WidgetBindingRadioGroupAdapter, WidgetDataSyncsVisibleDualOptionsWidget) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* frame = new QRadioButton(u"Frame"_s, &root);
  frame->setObjectName("frameMode");
  frame->setChecked(true);
  auto* arrow = new QRadioButton(u"Arrow"_s, &root);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(&root);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);

  PJ::adaptRadioGroups(&root);
  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  ASSERT_EQ(dual->selectedIndex(), 0);

  PJ::WidgetData wd;
  wd.setChecked("arrowMode", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_TRUE(arrow->isChecked());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsAfterInitialWidgetDataSelectsRadio) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* frame = new QRadioButton(u"Frame"_s, &root);
  frame->setObjectName("frameMode");
  auto* arrow = new QRadioButton(u"Arrow"_s, &root);
  arrow->setObjectName("arrowMode");
  auto* group = new QButtonGroup(&root);
  group->addButton(frame);
  group->addButton(arrow);
  row_layout->addWidget(frame);
  row_layout->addWidget(arrow);

  PJ::adaptRadioGroups(&root);
  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr)
      << "no-selection pairs stay untouched until plugin data chooses an option";

  PJ::WidgetData wd;
  wd.setChecked("arrowMode", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(frame->isHidden());
  EXPECT_TRUE(arrow->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsPairEmbeddedInMixedBoxRow) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* label = new QLabel(u"Timestamp:"_s, &root);
  auto* publish = new QRadioButton(u"publish"_s, &root);
  publish->setObjectName("publishTimestamp");
  publish->setChecked(true);
  auto* log = new QRadioButton(u"log"_s, &root);
  log->setObjectName("logTimestamp");
  auto* group = new QButtonGroup(&root);
  group->addButton(publish);
  group->addButton(log);
  auto* header = new QCheckBox(u"Use timestamp inside message (header)"_s, &root);
  row_layout->addWidget(label);
  row_layout->addWidget(publish);
  row_layout->addWidget(log);
  row_layout->addStretch();
  row_layout->addWidget(header);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
  EXPECT_FALSE(label->isHidden());
  EXPECT_FALSE(header->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 0);
}

TEST(WidgetBindingRadioGroupAdapter, LeavesUngroupedTwoRadioRowUntouched) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* first = new QRadioButton(u"First"_s, &root);
  first->setChecked(true);
  auto* second = new QRadioButton(u"Second"_s, &root);
  row_layout->addWidget(first);
  row_layout->addWidget(second);

  PJ::adaptRadioGroups(&root);

  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr);
  EXPECT_FALSE(first->isHidden());
  EXPECT_FALSE(second->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsButtonGroupsInsideNestedLayouts) {
  qapp();

  QWidget root;
  auto* outer_layout = new QVBoxLayout(&root);

  auto* array_row = new QHBoxLayout();
  auto* spin = new QSpinBox(&root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  auto* array_group = new QButtonGroup(&root);
  array_group->addButton(clamp);
  array_group->addButton(skip);
  skip->setChecked(true);
  array_row->addWidget(new QLabel(u"When an array size exceeds:"_s, &root));
  array_row->addWidget(spin);
  array_row->addStretch();
  array_row->addWidget(clamp);
  array_row->addWidget(skip);
  outer_layout->addLayout(array_row);

  auto* timestamp_row = new QHBoxLayout();
  auto* publish = new QRadioButton(u"publish"_s, &root);
  auto* log = new QRadioButton(u"log"_s, &root);
  auto* timestamp_group = new QButtonGroup(&root);
  timestamp_group->addButton(publish);
  timestamp_group->addButton(log);
  publish->setChecked(true);
  timestamp_row->addWidget(new QLabel(u"Timestamp:"_s, &root));
  timestamp_row->addWidget(publish);
  timestamp_row->addWidget(log);
  timestamp_row->addStretch();
  timestamp_row->addWidget(new QCheckBox(u"Use timestamp inside message (header)"_s, &root));
  outer_layout->addLayout(timestamp_row);

  PJ::adaptRadioGroups(&root);

  const auto duals = root.findChildren<PJ::DualOptionsWidget*>();
  ASSERT_EQ(duals.size(), 2);
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsIndependentButtonGroupsSharingParent) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* publish = new QRadioButton(u"publish"_s, &root);
  auto* log = new QRadioButton(u"log"_s, &root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  auto* timestamp_group = new QButtonGroup(&root);
  timestamp_group->addButton(publish);
  timestamp_group->addButton(log);
  auto* overflow_group = new QButtonGroup(&root);
  overflow_group->addButton(clamp);
  overflow_group->addButton(skip);
  publish->setChecked(true);
  skip->setChecked(true);
  row_layout->addWidget(new QLabel(u"Timestamp:"_s, &root));
  row_layout->addWidget(publish);
  row_layout->addWidget(log);
  row_layout->addStretch();
  row_layout->addWidget(new QLabel(u"When an array size exceeds:"_s, &root));
  row_layout->addWidget(clamp);
  row_layout->addWidget(skip);

  PJ::adaptRadioGroups(&root);

  const auto duals = root.findChildren<PJ::DualOptionsWidget*>();
  ASSERT_EQ(duals.size(), 2);
  EXPECT_TRUE(publish->isHidden());
  EXPECT_TRUE(log->isHidden());
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsGroupedPairInsideGridRow) {
  qapp();

  QWidget root;
  auto* grid = new QGridLayout(&root);
  auto* spin = new QSpinBox(&root);
  auto* clamp = new QRadioButton(u"Clamp"_s, &root);
  auto* skip = new QRadioButton(u"Skip"_s, &root);
  skip->setChecked(true);
  auto* overflow_group = new QButtonGroup(&root);
  overflow_group->addButton(clamp);
  overflow_group->addButton(skip);
  grid->addWidget(new QLabel(u"When an array size exceeds:"_s, &root), 0, 0);
  grid->addWidget(spin, 0, 1);
  grid->addItem(
      new QSpacerItem(
          PJ::theme::space(PJ::theme::Space::Section), PJ::theme::space(PJ::theme::Space::Tight),
          QSizePolicy::Expanding, QSizePolicy::Minimum),
      0, 2);
  grid->addWidget(clamp, 0, 3);
  grid->addWidget(skip, 0, 4);
  grid->addWidget(new QCheckBox(u"Use timestamp inside message (header)"_s, &root), 1, 0, 1, 5);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_TRUE(clamp->isHidden());
  EXPECT_TRUE(skip->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);
}

TEST(WidgetBindingRadioGroupAdapter, LeavesUngroupedLargerRadioSetUntouched) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* a = new QRadioButton(u"A"_s, &root);
  a->setChecked(true);
  auto* b = new QRadioButton(u"B"_s, &root);
  auto* c = new QRadioButton(u"C"_s, &root);
  auto* d = new QRadioButton(u"D"_s, &root);
  row_layout->addWidget(a);
  row_layout->addWidget(b);
  row_layout->addWidget(c);
  row_layout->addWidget(d);

  PJ::adaptRadioGroups(&root);

  EXPECT_EQ(root.findChild<PJ::DualOptionsWidget*>(), nullptr);
  EXPECT_FALSE(a->isHidden());
  EXPECT_FALSE(b->isHidden());
  EXPECT_FALSE(c->isHidden());
  EXPECT_FALSE(d->isHidden());
}

TEST(WidgetBindingRadioGroupAdapter, ConvertsExplicitThreeButtonGroup) {
  qapp();

  QWidget root;
  auto* row_layout = new QHBoxLayout(&root);
  auto* contains = new QRadioButton(u"Contains"_s, &root);
  contains->setObjectName("filterContains");
  auto* wildcard = new QRadioButton(u"Wildcard"_s, &root);
  wildcard->setObjectName("filterWildcard");
  wildcard->setChecked(true);
  auto* regexp = new QRadioButton(u"RegExp"_s, &root);
  regexp->setObjectName("filterRegExp");
  auto* group = new QButtonGroup(&root);
  group->addButton(contains);
  group->addButton(wildcard);
  group->addButton(regexp);
  row_layout->addWidget(contains);
  row_layout->addWidget(wildcard);
  row_layout->addWidget(regexp);

  PJ::adaptRadioGroups(&root);

  auto* dual = root.findChild<PJ::DualOptionsWidget*>();
  ASSERT_NE(dual, nullptr);
  EXPECT_EQ(dual->optionCount(), 3);
  EXPECT_TRUE(contains->isHidden());
  EXPECT_TRUE(wildcard->isHidden());
  EXPECT_TRUE(regexp->isHidden());
  EXPECT_EQ(dual->selectedIndex(), 1);

  dual->setSelectedIndex(2);
  EXPECT_TRUE(regexp->isChecked());
  EXPECT_FALSE(wildcard->isChecked());

  PJ::WidgetData wd;
  wd.setChecked("filterContains", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));
  EXPECT_EQ(dual->selectedIndex(), 0);
  EXPECT_TRUE(contains->isHidden());
}

TEST(WidgetCheckBoxAdapter, ConvertsCheckBoxToLabeledToggleAndPreservesEvents) {
  qapp();
  recorder()->clear();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Enable streaming"_s, &root);
  check->setObjectName("enableStreaming");
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  auto* toggle = root.findChild<PJ::ToggleSwitch*>();
  ASSERT_NE(toggle, nullptr);
  EXPECT_TRUE(check->isHidden());
  EXPECT_EQ(toggle->text(), u"Enable streaming"_s);
  EXPECT_EQ(toggle->labelSide(), PJ::ToggleSwitch::LabelSide::Left);
  EXPECT_FALSE(toggle->isChecked());

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  // Flip the toggle; it emits `toggled` only once its slide animation settles
  // (iOS-switch semantics), so wait for it, then assert the hidden checkbox
  // followed AND drove the plugin callback.
  toggle->setChecked(true);
  QTest::qWait(300);

  EXPECT_TRUE(check->isChecked());
  bool saw_checked = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "enableStreaming") {
      continue;
    }
    const auto j = nlohmann::json::parse(ev.json, nullptr, false);
    saw_checked = !j.is_discarded() && j.value("checked", false);
  }
  EXPECT_TRUE(saw_checked) << "the hidden original checkbox must still drive plugin onToggled callbacks";
}

TEST(WidgetCheckBoxAdapter, WidgetDataSyncsToggle) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Loop"_s, &root);
  check->setObjectName("loop");
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);
  auto* toggle = root.findChild<PJ::ToggleSwitch*>();
  ASSERT_NE(toggle, nullptr);
  ASSERT_FALSE(toggle->isChecked());

  PJ::WidgetData wd;
  wd.setChecked("loop", true);
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(check->isHidden());
  EXPECT_TRUE(check->isChecked());
  EXPECT_TRUE(toggle->isChecked());
}

TEST(WidgetCheckBoxAdapter, LeavesTristateCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(u"Partial"_s, &root);
  check->setTristate(true);
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  EXPECT_EQ(root.findChild<PJ::ToggleSwitch*>(), nullptr);
  EXPECT_FALSE(check->isHidden());
}

TEST(WidgetCheckBoxAdapter, LeavesTextlessCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* check = new QCheckBox(&root);  // no label
  layout->addWidget(check);

  PJ::adaptCheckBoxes(&root);

  EXPECT_EQ(root.findChild<PJ::ToggleSwitch*>(), nullptr);
  EXPECT_FALSE(check->isHidden());
}

TEST(WidgetCheckBoxAdapter, LeavesHostCompositeInternalCheckBoxUntouched) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  // CredentialsEditor owns an internal "allow insecure" checkbox; the adapter
  // must treat composites as opaque and leave their internals alone.
  auto* creds = new PJ::CredentialsEditor(&root);
  layout->addWidget(creds);

  PJ::adaptCheckBoxes(&root);

  for (auto* cb : creds->findChildren<QCheckBox*>()) {
    EXPECT_EQ(cb->property("_pj_toggle_switch").value<QObject*>(), nullptr);
    EXPECT_FALSE(cb->isHidden());
  }
}

TEST(WidgetComboBoxAdapter, UpgradesPlainComboBoxInPlacePreservingState) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* combo = new QComboBox(&root);
  combo->setObjectName("mode");
  combo->addItems({u"a"_s, u"b"_s, u"c"_s});
  combo->setCurrentIndex(2);
  layout->addWidget(combo);

  PJ::adaptComboBoxes(&root);

  // Same widget object — model + current index survive (no swap).
  EXPECT_EQ(root.findChild<QComboBox*>("mode"), combo);
  EXPECT_EQ(combo->count(), 3);
  EXPECT_EQ(combo->currentIndex(), 2);
  // Gradient delegate now installed.
  EXPECT_NE(qobject_cast<PJ::ComboBoxGradientDelegate*>(combo->itemDelegate()), nullptr);
}

TEST(WidgetComboBoxAdapter, LeavesPromotedComboBoxStyled) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* combo = new PJ::ComboBox(&root);  // already promoted in the .ui
  layout->addWidget(combo);

  PJ::adaptComboBoxes(&root);

  // Still its own class, still gradient-styled (from its constructor).
  EXPECT_NE(qobject_cast<PJ::ComboBox*>(root.findChild<QComboBox*>()), nullptr);
  EXPECT_NE(qobject_cast<PJ::ComboBoxGradientDelegate*>(combo->itemDelegate()), nullptr);
}

// Full-width tabs contract (dexory_cloud_panel.ui "filterTabs"): a tab bar only
// gets the whole pane width in documentMode, and Qt's setDocumentMode(true)
// resets QTabBar::expanding to false during the .ui load — so the QTabWidget
// binding must re-assert expanding on apply or document-mode tabs silently
// stop stretching. Pins both halves of that sequence.
TEST(WidgetBindingTabWidget, DocumentModeSurvivesLoadAndApplyRestoresExpanding) {
  qapp();
  const QByteArray ui = R"(<?xml version="1.0" encoding="UTF-8"?>
<ui version="4.0">
 <class>Root</class>
 <widget class="QWidget" name="Root">
  <layout class="QVBoxLayout">
   <item>
    <widget class="QTabWidget" name="filterTabs">
     <property name="documentMode"><bool>true</bool></property>
     <widget class="QWidget" name="basicTab">
      <attribute name="title"><string>Basic</string></attribute>
      <layout class="QVBoxLayout"/>
     </widget>
     <widget class="QWidget" name="advancedTab">
      <attribute name="title"><string>Advanced</string></attribute>
      <layout class="QVBoxLayout"/>
     </widget>
    </widget>
   </item>
  </layout>
 </widget>
</ui>)";
  QByteArray data(ui);
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);
  PJ::PjUiLoader loader;
  QWidget* root = loader.load(&buffer);
  ASSERT_NE(root, nullptr);
  auto* tabs = root->findChild<QTabWidget*>("filterTabs");
  ASSERT_NE(tabs, nullptr);
  EXPECT_TRUE(tabs->documentMode()) << "QUiLoader must honor the .ui documentMode property";
  EXPECT_FALSE(tabs->tabBar()->expanding()) << "precondition: setDocumentMode(true) resets expanding";
  EXPECT_TRUE(tabs->tabBar()->drawBase()) << "precondition: Qt defaults to drawing the tab-bar base";

  PJ::WidgetData wd;
  wd.setTabIndex("filterTabs", 1);
  PJ::applyWidgetData(root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_TRUE(tabs->tabBar()->expanding()) << "apply must re-assert expanding after the documentMode reset";
  EXPECT_FALSE(tabs->tabBar()->drawBase())
      << "apply must drop the document-mode base line (stray line over the unselected tab)";
  EXPECT_EQ(tabs->currentIndex(), 1);
  delete root;
}

// --- adaptScrollAreas --------------------------------------------------------

// A QScrollArea under root gets one H + one V PJ::Scrollbar attached, the
// native bars are forced to AlwaysOff, and a second call produces no duplicates.
TEST(WidgetScrollAreaAdapter, AttachesHAndVScrollbarsAndHidesNativeBars) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  const auto scrollbars = root.findChildren<PJ::Scrollbar*>();
  ASSERT_EQ(scrollbars.size(), 2) << "expect one H + one V Scrollbar per area";

  EXPECT_EQ(area->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff)
      << "attach() must hide the native horizontal bar";
  EXPECT_EQ(area->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff) << "attach() must hide the native vertical bar";

  // Idempotent: a second call must not add more scrollbars.
  PJ::adaptScrollAreas(&root);
  EXPECT_EQ(root.findChildren<PJ::Scrollbar*>().size(), 2)
      << "pjScrollbarAttached marker must prevent duplicate overlays";
}

// A pjScrollbarAutoHide=false property on the area propagates to both pills,
// which must transition immediately to the shown state (full opacity).
TEST(WidgetScrollAreaAdapter, PropagatesAutoHideFalseConfig) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  area->setProperty("pjScrollbarAutoHide", false);
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  const auto scrollbars = root.findChildren<PJ::Scrollbar*>();
  ASSERT_EQ(scrollbars.size(), 2);
  for (auto* sb : scrollbars) {
    EXPECT_TRUE(sb->isShown()) << "auto-hide=false must force the pill to the shown state immediately";
  }
}

// A plugin that pinned an axis to ScrollBarAlwaysOn wants a persistent native
// bar there; adaptScrollAreas must skip that axis (no pill) and leave its policy
// untouched, while still adapting the other (default) axis.
TEST(WidgetScrollAreaAdapter, RespectsAlwaysOnPolicyPerAxis) {
  qapp();

  QWidget root;
  auto* layout = new QVBoxLayout(&root);
  auto* area = new QScrollArea(&root);
  auto* inner = new QWidget();
  inner->setMinimumSize(2000, 2000);
  area->setWidget(inner);
  area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);  // plugin wants the native V bar
  layout->addWidget(area);

  PJ::adaptScrollAreas(&root);

  EXPECT_EQ(root.findChildren<PJ::Scrollbar*>().size(), 1) << "only the horizontal (default) axis is adapted";
  EXPECT_EQ(area->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn) << "pinned AlwaysOn vertical bar is left untouched";
  EXPECT_EQ(area->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff) << "default horizontal axis still gets a pill";
}

// setDateTime/setDateTimeRange land on a QDateTimeEdit (range first, so the
// value is not clamped by a stale default range).
TEST(WidgetBindingDateTime, AppliesValueAndRange) {
  qapp();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("startTime");

  PJ::WidgetData wd;
  wd.setDateTime("startTime", "2026-05-21T13:45:00");
  wd.setDateTimeRange("startTime", "2026-05-01T00:00:00", "2026-06-01T00:00:00");
  PJ::applyWidgetData(&root, PJ::WidgetDataView(wd.toJson()));

  EXPECT_EQ(edit->dateTime(), QDateTime::fromString(u"2026-05-21T13:45:00"_s, Qt::ISODate));
  EXPECT_EQ(edit->minimumDateTime(), QDateTime::fromString(u"2026-05-01T00:00:00"_s, Qt::ISODate));
  EXPECT_EQ(edit->maximumDateTime(), QDateTime::fromString(u"2026-06-01T00:00:00"_s, Qt::ISODate));
}

// An edited QDateTimeEdit reports back as a datetime_iso event so the typed
// dispatcher routes it to onDateTimeChanged.
TEST(WidgetBindingDateTime, UserEditEmitsDateTimeIso) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("startTime");

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  edit->setDateTime(QDateTime::fromString(u"2026-01-02T03:04:05"_s, Qt::ISODate));

  bool saw_datetime = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "startTime") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("datetime_iso") && j["datetime_iso"] == "2026-01-02T03:04:05") {
      saw_datetime = true;
    }
  }
  EXPECT_TRUE(saw_datetime) << "QDateTimeEdit edit must emit a datetime_iso event";
}

// Editors whose display format carries milliseconds must round-trip them
// (the event serializes with ISODateWithMs; whole-second values stay bare).
TEST(WidgetBindingDateTime, MillisecondEditorEmitsFractionalSeconds) {
  qapp();
  recorder()->clear();
  QWidget root;
  auto* edit = new QDateTimeEdit(&root);
  edit->setObjectName("stamp");
  edit->setDisplayFormat(u"yyyy-MM-dd HH:mm:ss.zzz"_s);

  PJ::connectWidgetSignals(
      &root, [](const std::string& name, const std::string& json) { recorder()->push_back({name, json}); });

  edit->setDateTime(QDateTime::fromString(u"2026-01-02T03:04:05.678"_s, Qt::ISODateWithMs));

  bool saw_ms = false;
  for (const auto& ev : *recorder()) {
    if (ev.name != "stamp") {
      continue;
    }
    auto j = nlohmann::json::parse(ev.json, nullptr, false);
    if (!j.is_discarded() && j.contains("datetime_iso") && j["datetime_iso"] == "2026-01-02T03:04:05.678") {
      saw_ms = true;
    }
  }
  EXPECT_TRUE(saw_ms) << "ms-precision editors must not truncate fractional seconds";
}

}  // namespace
