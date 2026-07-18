// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <map>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/dialog_engine.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
using namespace Qt::StringLiterals;

// Defined in mock_dialog.cpp, linked statically
extern "C" const PJ_dialog_vtable_t* PJ_get_dialog_vtable() noexcept;

// ==========================================================================
// Widget Binding Tests — programmatic widgets, no QUiLoader needed
// ==========================================================================

class WidgetBindingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = new QWidget();
    auto* layout = new QVBoxLayout(root_);

    line_edit_ = new QLineEdit(root_);
    line_edit_->setObjectName("name_input");
    layout->addWidget(line_edit_);

    spin_box_ = new QSpinBox(root_);
    spin_box_->setObjectName("count_input");
    spin_box_->setRange(0, 99999);
    layout->addWidget(spin_box_);

    check_box_ = new QCheckBox(root_);
    check_box_->setObjectName("verbose_check");
    layout->addWidget(check_box_);

    button_box_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, root_);
    button_box_->setObjectName("buttonBox");
    layout->addWidget(button_box_);
  }

  void TearDown() override {
    delete root_;
  }

  QWidget* root_ = nullptr;
  QLineEdit* line_edit_ = nullptr;
  QSpinBox* spin_box_ = nullptr;
  QCheckBox* check_box_ = nullptr;
  QDialogButtonBox* button_box_ = nullptr;
};

// --- apply_widget_data ---

TEST_F(WidgetBindingTest, ApplyText) {
  nlohmann::json data;
  data["name_input"]["text"] = "my_source";
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_EQ(line_edit_->text().toStdString(), "my_source");
}

TEST_F(WidgetBindingTest, ApplySpinBoxValue) {
  nlohmann::json data;
  data["count_input"]["value"] = 42;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_EQ(spin_box_->value(), 42);
}

TEST_F(WidgetBindingTest, ApplySpinBoxRange) {
  nlohmann::json data;
  data["count_input"]["min"] = 0;
  data["count_input"]["max"] = 1000;
  data["count_input"]["value"] = 500;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_EQ(spin_box_->minimum(), 0);
  EXPECT_EQ(spin_box_->maximum(), 1000);
  EXPECT_EQ(spin_box_->value(), 500);
}

TEST_F(WidgetBindingTest, ApplyCheckBox) {
  nlohmann::json data;
  data["verbose_check"]["checked"] = true;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_TRUE(check_box_->isChecked());
}

TEST_F(WidgetBindingTest, ApplyOkEnabled) {
  nlohmann::json data;
  data["buttonBox"]["ok_enabled"] = false;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  auto* ok = button_box_->button(QDialogButtonBox::Ok);
  ASSERT_NE(ok, nullptr);
  EXPECT_FALSE(ok->isEnabled());
}

TEST_F(WidgetBindingTest, ApplyVisibility) {
  nlohmann::json data;
  data["count_input"]["visible"] = false;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_FALSE(spin_box_->isVisible());
}

TEST_F(WidgetBindingTest, ApplyEnabled) {
  nlohmann::json data;
  data["name_input"]["enabled"] = false;
  PJ::WidgetDataView view(data.dump());

  PJ::applyWidgetData(root_, view);
  EXPECT_FALSE(line_edit_->isEnabled());
}

TEST_F(WidgetBindingTest, MissingWidgetIsIgnored) {
  nlohmann::json data;
  data["nonexistent"]["text"] = "whatever";
  PJ::WidgetDataView view(data.dump());

  // Should not crash
  PJ::applyWidgetData(root_, view);
}

// --- connect_widget_signals ---

TEST_F(WidgetBindingTest, SignalTextChanged) {
  std::string captured_name;
  std::string captured_json;
  PJ::connectWidgetSignals(root_, [&](const std::string& name, const std::string& json) {
    captured_name = name;
    captured_json = json;
  });

  line_edit_->setText("new_name");

  EXPECT_EQ(captured_name, "name_input");
  auto j = nlohmann::json::parse(captured_json);
  EXPECT_EQ(j["text"], "new_name");
}

TEST_F(WidgetBindingTest, SignalSpinBoxValueChanged) {
  std::string captured_name;
  std::string captured_json;
  PJ::connectWidgetSignals(root_, [&](const std::string& name, const std::string& json) {
    captured_name = name;
    captured_json = json;
  });

  spin_box_->setValue(99);

  EXPECT_EQ(captured_name, "count_input");
  auto j = nlohmann::json::parse(captured_json);
  EXPECT_EQ(j["value"], 99);
}

TEST_F(WidgetBindingTest, SignalCheckBoxToggled) {
  std::string captured_name;
  std::string captured_json;
  PJ::connectWidgetSignals(root_, [&](const std::string& name, const std::string& json) {
    captured_name = name;
    captured_json = json;
  });

  check_box_->setChecked(true);

  EXPECT_EQ(captured_name, "verbose_check");
  auto j = nlohmann::json::parse(captured_json);
  EXPECT_EQ(j["checked"], true);
}

TEST_F(WidgetBindingTest, SignalBlockerPreventsReentrant) {
  int call_count = 0;
  PJ::connectWidgetSignals(root_, [&](const std::string&, const std::string&) { call_count++; });

  // apply_widget_data should NOT trigger signals (QSignalBlocker)
  nlohmann::json data;
  data["name_input"]["text"] = "blocked-text";
  PJ::applyWidgetData(root_, PJ::WidgetDataView(data.dump()));

  EXPECT_EQ(call_count, 0);
  EXPECT_EQ(line_edit_->text().toStdString(), "blocked-text");
}

// ==========================================================================
// DialogEngine Tests — headless (no QDialog shown)
// ==========================================================================

class DialogEngineTest : public ::testing::Test {
 protected:
  const PJ_dialog_vtable_t* vt_ = PJ_get_dialog_vtable();
};

TEST_F(DialogEngineTest, RunHeadlessReturnWidgetData) {
  PJ::DialogHandle handle(vt_);
  PJ::DialogEngine engine(std::move(handle));

  std::string result = engine.runHeadless(0);
  auto j = nlohmann::json::parse(result, nullptr, false);
  EXPECT_FALSE(j.is_discarded());
  EXPECT_TRUE(j.contains("name_input"));
}

TEST_F(DialogEngineTest, RunHeadlessWithTicks) {
  PJ::DialogHandle handle(vt_);
  PJ::DialogEngine engine(std::move(handle));

  // mock_dialog has no tick behavior, but should still return valid widget data
  std::string result = engine.runHeadless(5);
  auto j = nlohmann::json::parse(result);
  EXPECT_TRUE(j.contains("name_input"));
  EXPECT_TRUE(j.contains("count_input"));
}

TEST_F(DialogEngineTest, ConfigCanBeSet) {
  PJ::DialogEngineConfig config;
  config.tick_interval_ms = 100;
  config.enable_diff = false;
  config.enable_file_picker = false;

  PJ::DialogHandle handle(vt_);
  PJ::DialogEngine engine(std::move(handle), config);

  // Should not crash — just validates config is stored
  std::string result = engine.runHeadless(1);
  EXPECT_FALSE(result.empty());
}

TEST_F(DialogEngineTest, SavedConfigReturnsPluginConfig) {
  PJ::DialogHandle handle(vt_);
  (void)handle.sendEvent("name_input", R"({"text": "config_test"})");
  PJ::DialogEngine engine(std::move(handle));

  std::string cfg = engine.savedConfig();
  auto j = nlohmann::json::parse(cfg);
  EXPECT_EQ(j["name"], "config_test");
}

// ==========================================================================
// Full round-trip: widget binding + mock_dialog through DialogHandle
// ==========================================================================

TEST_F(DialogEngineTest, RoundTripWidgetBinding) {
  PJ::DialogHandle handle(vt_);

  // Get initial widget data and apply to programmatic widgets
  PJ::WidgetDataView view(handle.widget_data());

  auto* root = new QWidget();
  auto* layout = new QVBoxLayout(root);

  auto* line_edit = new QLineEdit(root);
  line_edit->setObjectName("name_input");
  layout->addWidget(line_edit);

  auto* spin_box = new QSpinBox(root);
  spin_box->setObjectName("count_input");
  spin_box->setRange(0, 99999);
  layout->addWidget(spin_box);

  PJ::applyWidgetData(root, view);

  // Verify the mock_dialog's initial state was applied
  EXPECT_EQ(line_edit->text().toStdString(), "default");
  EXPECT_EQ(spin_box->value(), 10);

  // Simulate user editing name via signal
  std::string captured_name;
  std::string captured_json;
  PJ::connectWidgetSignals(root, [&](const std::string& name, const std::string& json) {
    captured_name = name;
    captured_json = json;
  });

  line_edit->setText("my_source");
  EXPECT_EQ(captured_name, "name_input");

  // Send event to plugin
  bool refresh = handle.sendEvent(captured_name, captured_json);
  EXPECT_TRUE(refresh);

  // Re-read and apply updated widget data
  PJ::WidgetDataView updated_view(handle.widget_data());
  PJ::applyWidgetData(root, updated_view);

  // Verify name was updated in the plugin's state
  EXPECT_EQ(updated_view.text("name_input").value_or(""), "my_source");

  delete root;
}

// ==========================================================================
// Table binding — selection changes must not rebuild rows
// ==========================================================================

namespace {
nlohmann::json tableData(
    const char* name, const std::vector<std::vector<std::string>>& rows, const std::vector<int>& selected) {
  nlohmann::json d;
  d[name]["headers"] = {"A", "B"};
  d[name]["rows"] = rows;
  d[name]["selected_rows"] = selected;
  return d;
}

// Read a two-column table back into a name→value map, asserting no cell is null.
// EXPECT (not ASSERT) so a null cell still reports its row while the caller's own
// ASSERT_EQ(rowCount) guards the loop bound.
std::map<std::string, std::string> gatherRowPairs(QTableWidget* tw, int row_count) {
  std::map<std::string, std::string> pairs;
  for (int r = 0; r < row_count; ++r) {
    EXPECT_NE(tw->item(r, 0), nullptr) << "row " << r;
    EXPECT_NE(tw->item(r, 1), nullptr) << "row " << r;
    if (tw->item(r, 0) != nullptr && tw->item(r, 1) != nullptr) {
      pairs[tw->item(r, 0)->text().toStdString()] = tw->item(r, 1)->text().toStdString();
    }
  }
  return pairs;
}
}  // namespace

// Selecting a row re-delivers identical rows under the same widget-data key.
// The host must reuse the existing QTableWidgetItems (cheap) rather than
// recreating them — this is what kept row clicks from rebuilding the table.
TEST(TableBinding, SelectionChangeReusesItems) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::vector<std::vector<std::string>> rows = {{"a0", "b0"}, {"a1", "b1"}};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", rows, {0}).dump()));

  ASSERT_EQ(tw->rowCount(), 2);
  QTableWidgetItem* item00 = tw->item(0, 0);
  QTableWidgetItem* item11 = tw->item(1, 1);
  ASSERT_NE(item00, nullptr);
  ASSERT_NE(item11, nullptr);
  EXPECT_TRUE(tw->item(0, 0)->isSelected());

  // Same rows, selection moves to row 1.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", rows, {1}).dump()));

  EXPECT_EQ(tw->item(0, 0), item00);  // same pointer ⇒ not rebuilt
  EXPECT_EQ(tw->item(1, 1), item11);
  EXPECT_FALSE(tw->item(0, 0)->isSelected());
  EXPECT_TRUE(tw->item(1, 0)->isSelected());
}

TEST(TableBinding, RowContentChangeRebuildsCells) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a0", "b0"}}, {}).dump()));
  ASSERT_EQ(tw->rowCount(), 1);
  EXPECT_EQ(tw->item(0, 0)->text(), "a0");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"X", "Y"}, {"Z", "W"}}, {}).dump()));
  ASSERT_EQ(tw->rowCount(), 2);
  EXPECT_EQ(tw->item(0, 0)->text(), "X");
  EXPECT_EQ(tw->item(1, 1)->text(), "W");
}

// Filtering: visible_rows hides the rows not in the set, and re-applying a wider
// set (same rows ⇒ rebuild skipped) must still un-hide them. Without honoring
// visible_rows the date/name/query filters do nothing.
TEST(TableBinding, VisibleRowsHideAndShowFilteredRows) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::vector<std::vector<std::string>> rows = {{"a0", "b0"}, {"a1", "b1"}, {"a2", "b2"}};
  auto with_visible = [&](const std::vector<int>& visible) {
    nlohmann::json d;
    d["tbl"]["headers"] = {"A", "B"};
    d["tbl"]["rows"] = rows;
    d["tbl"]["visible_rows"] = visible;
    return d.dump();
  };

  PJ::applyWidgetData(&root, PJ::WidgetDataView(with_visible({1})));
  ASSERT_EQ(tw->rowCount(), 3);
  EXPECT_TRUE(tw->isRowHidden(0));
  EXPECT_FALSE(tw->isRowHidden(1));
  EXPECT_TRUE(tw->isRowHidden(2));

  // Widen the filter — rows unchanged, so the item rebuild is skipped, but the
  // hidden state must still update.
  PJ::applyWidgetData(&root, PJ::WidgetDataView(with_visible({0, 1, 2})));
  EXPECT_FALSE(tw->isRowHidden(0));
  EXPECT_FALSE(tw->isRowHidden(1));
  EXPECT_FALSE(tw->isRowHidden(2));
}

// Streaming parity: when a sequence's detail arrives, only that cell changes and
// the row count is unchanged, so the item is updated in place (pointer kept)
// rather than rebuilding the table — this is what makes detail fill in row by
// row instead of all at once.
TEST(TableBinding, SameShapeCellUpdateIsInPlace) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a0", "--"}, {"a1", "--"}}, {}).dump()));
  ASSERT_EQ(tw->rowCount(), 2);
  QTableWidgetItem* item01 = tw->item(0, 1);
  ASSERT_NE(item01, nullptr);
  EXPECT_EQ(item01->text(), "--");

  // Row 0's detail streams in (same shape, one cell changes).
  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a0", "42 MiB"}, {"a1", "--"}}, {}).dump()));
  EXPECT_EQ(tw->item(0, 1), item01);  // same item ⇒ updated in place
  EXPECT_EQ(item01->text(), "42 MiB");
  EXPECT_EQ(tw->item(1, 1)->text(), "--");
}

// selected_items (text-keyed restore, setSelectedItems) must be honored for
// QTableWidget the same way it already is for QListWidget — plugins with
// sortable picker tables emit it instead of index-keyed selected_rows.
TEST(TableBinding, SelectedItemsRestoreByText) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::vector<std::vector<std::string>> rows = {{"a0", "b0"}, {"a1", "b1"}};
  nlohmann::json d;
  d["tbl"]["headers"] = {"A", "B"};
  d["tbl"]["rows"] = rows;
  d["tbl"]["selected_items"] = {"a1"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  ASSERT_EQ(tw->rowCount(), 2);
  EXPECT_FALSE(tw->item(0, 0)->isSelected());
  EXPECT_TRUE(tw->item(1, 0)->isSelected());
}

// The point of text-keyed restore: it must land on the right row even after the
// view re-orders (sortingEnabled) — index-keyed selected_rows desyncs here.
TEST(TableBinding, SelectedItemsSurviveSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  nlohmann::json d;
  d["tbl"]["headers"] = {"A", "B"};
  d["tbl"]["rows"] = std::vector<std::vector<std::string>>{{"a0", "b0"}, {"a1", "b1"}};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  ASSERT_EQ(tw->rowCount(), 2);

  // Re-order the view the way a user header-click would.
  tw->sortItems(0, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 0)->text(), "a1");  // sorted: a1 now on top

  nlohmann::json sel;
  sel["tbl"]["selected_items"] = {"a0"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(sel.dump()));

  // "a0" lives at visual row 1 after the sort — text-keyed restore must find it.
  EXPECT_TRUE(tw->item(1, 0)->isSelected());
  EXPECT_FALSE(tw->item(0, 0)->isSelected());
}

// Same reuse guarantee the index-keyed path has: a selection-only re-delivery
// must not rebuild the QTableWidgetItems.
TEST(TableBinding, SelectedItemsChangeReusesItems) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");

  const std::vector<std::vector<std::string>> rows = {{"a0", "b0"}, {"a1", "b1"}};
  nlohmann::json d;
  d["tbl"]["headers"] = {"A", "B"};
  d["tbl"]["rows"] = rows;
  d["tbl"]["selected_items"] = {"a0"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  QTableWidgetItem* item00 = tw->item(0, 0);
  ASSERT_NE(item00, nullptr);
  EXPECT_TRUE(item00->isSelected());

  d["tbl"]["selected_items"] = {"a1"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  EXPECT_EQ(tw->item(0, 0), item00);  // same pointer ⇒ not rebuilt
  EXPECT_FALSE(tw->item(0, 0)->isSelected());
  EXPECT_TRUE(tw->item(1, 0)->isSelected());
}

// A picker table with sortingEnabled=true must survive being populated. Rows
// arrive in the plugin's order; the host writes them by model-row index. If
// sorting is not suspended during the write, QTableWidget re-sorts the model
// mid-loop and the remaining writes land on the wrong rows.

// Populate while sorting is active but no sort indicator has been chosen yet.
TEST(TableBinding, SortingActivePopulateKeepsRowIntegrity) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"c", "3"}, {"a", "1"}, {"b", "2"}}, {}).dump()));

  ASSERT_EQ(tw->rowCount(), 3);
  const auto pairs = gatherRowPairs(tw, 3);
  EXPECT_EQ(pairs.at("a"), "1");
  EXPECT_EQ(pairs.at("b"), "2");
  EXPECT_EQ(pairs.at("c"), "3");
}

// After the user sorts (active sort indicator), a shape change forces a full
// rebuild. Without suspending sorting the rebuild leaves NULL cells.
TEST(TableBinding, SortedViewFullRebuildKeepsRowIntegrity) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"c", "3"}, {"a", "1"}}, {}).dump()));
  tw->sortItems(0, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(0, Qt::DescendingOrder);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"c", "3"}, {"a", "1"}, {"b", "2"}}, {}).dump()));

  ASSERT_EQ(tw->rowCount(), 3);
  const auto pairs = gatherRowPairs(tw, 3);
  EXPECT_EQ(pairs.at("a"), "1");
  EXPECT_EQ(pairs.at("b"), "2");
  EXPECT_EQ(pairs.at("c"), "3");
}

// Same-shape in-place update where a sort-column cell changes text. Without
// suspending sorting the in-place setText re-sorts mid-loop and duplicates a row.
TEST(TableBinding, SortedViewInPlaceTextUpdateKeepsRowIntegrity) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a", "1"}, {"b", "2"}, {"c", "3"}}, {}).dump()));
  tw->sortItems(0, Qt::AscendingOrder);
  tw->horizontalHeader()->setSortIndicator(0, Qt::AscendingOrder);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"z", "1"}, {"b", "2"}, {"c", "3"}}, {}).dump()));

  ASSERT_EQ(tw->rowCount(), 3);
  const auto pairs = gatherRowPairs(tw, 3);
  EXPECT_EQ(pairs.at("z"), "1");
  EXPECT_EQ(pairs.at("b"), "2");
  EXPECT_EQ(pairs.at("c"), "3");
}

// The protocol has no cell-edit event, so an editable cell would silently drop
// the edit. The host forces every table read-only regardless of what the .ui
// declared (here the QTableWidget defaults to editable).
TEST(TableBinding, RowsFedTableIsForcedReadOnly) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  ASSERT_NE(tw->editTriggers(), QAbstractItemView::NoEditTriggers);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a", "1"}, {"b", "2"}}, {}).dump()));

  EXPECT_EQ(tw->editTriggers(), QAbstractItemView::NoEditTriggers);
}

// ==========================================================================
// Table binding — index-keyed state must survive a user-sorted view
//
// Plugins compute row indices (selected_rows, visible_rows, disabled_rows,
// radio_checked_row) against the order they delivered `rows` in, and interpret
// emitted indices (table_radio_row, item_double_clicked_index) the same way.
// The host must translate plugin order ↔ view order once sortingEnabled lets
// the user re-order the view, or every index-keyed exchange lands on the
// wrong rows.
// ==========================================================================

namespace {

// View row whose cell at `col` shows `text`; -1 when absent.
int rowOfText(QTableWidget* tw, int col, const char* text) {
  for (int r = 0; r < tw->rowCount(); ++r) {
    if (auto* item = tw->item(r, col); item != nullptr && item->text() == text) {
      return r;
    }
  }
  return -1;
}

// Last int emitted under `key` for widget `name` across the captured events;
// nullopt when no such event was captured.
std::optional<int> lastEmittedInt(
    const std::vector<std::pair<std::string, std::string>>& events, const std::string& name, const char* key) {
  std::optional<int> out;
  for (const auto& [event_name, json] : events) {
    auto j = nlohmann::json::parse(json, nullptr, false);
    if (event_name == name && !j.is_discarded() && j.contains(key)) {
      out = j[key].get<int>();
    }
  }
  return out;
}

// View row whose radio cell at `col` is checked; -1 when none.
int checkedRadioRow(QTableWidget* tw, int col) {
  for (int r = 0; r < tw->rowCount(); ++r) {
    if (auto* radio = qobject_cast<QRadioButton*>(tw->cellWidget(r, col)); radio != nullptr && radio->isChecked()) {
      return r;
    }
  }
  return -1;
}

// Deliver rows a/b/c in plugin order, then re-order the view the way a user
// header-click would (descending ⇒ view shows c,b,a).
void deliverAbcAndSortDescending(QWidget* root, QTableWidget* tw) {
  PJ::applyWidgetData(root, PJ::WidgetDataView(tableData("tbl", {{"a", "1"}, {"b", "2"}, {"c", "3"}}, {}).dump()));
  ASSERT_EQ(tw->rowCount(), 3);
  tw->sortItems(0, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(0, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 0)->text(), "c");
}

}  // namespace

// visible_rows carries plugin-order indices: {0,1} means "the rows delivered
// as a and b", regardless of where the sort put them.
TEST(TableBinding, VisibleRowsRespectSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);
  deliverAbcAndSortDescending(&root, tw);

  nlohmann::json d;
  d["tbl"]["visible_rows"] = {0, 1};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_FALSE(tw->isRowHidden(rowOfText(tw, 0, "a")));
  EXPECT_FALSE(tw->isRowHidden(rowOfText(tw, 0, "b")));
  EXPECT_TRUE(tw->isRowHidden(rowOfText(tw, 0, "c")));
}

// disabled_rows={0} means "the row delivered first" (a), not view row 0.
TEST(TableBinding, DisabledRowsRespectSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);
  deliverAbcAndSortDescending(&root, tw);

  nlohmann::json d;
  d["tbl"]["disabled_rows"] = {0};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_FALSE(tw->item(rowOfText(tw, 0, "a"), 0)->flags() & Qt::ItemIsEnabled);
  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "b"), 0)->flags() & Qt::ItemIsEnabled);
  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "c"), 0)->flags() & Qt::ItemIsEnabled);
}

// cell_tooltips keyed "0,0" means "the row delivered first" (a), not view row 0.
TEST(TableBinding, CellTooltipsRespectSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);
  deliverAbcAndSortDescending(&root, tw);

  nlohmann::json d;
  d["tbl"]["cell_tooltips"]["0,0"] = "locked";
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_EQ(tw->item(rowOfText(tw, 0, "a"), 0)->toolTip(), "locked");
  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "b"), 0)->toolTip().isEmpty());
  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "c"), 0)->toolTip().isEmpty());
}

// A delivery that carries cell_tooltips states the complete set: a tooltip the
// plugin dropped must not linger on its old cell.
TEST(TableBinding, CellTooltipsDeliveryReplacesStaleOnes) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);
  deliverAbcAndSortDescending(&root, tw);

  nlohmann::json first;
  first["tbl"]["cell_tooltips"]["0,0"] = "locked";
  PJ::applyWidgetData(&root, PJ::WidgetDataView(first.dump()));
  ASSERT_EQ(tw->item(rowOfText(tw, 0, "a"), 0)->toolTip(), "locked");

  nlohmann::json second;
  second["tbl"]["cell_tooltips"]["1,0"] = "other";
  PJ::applyWidgetData(&root, PJ::WidgetDataView(second.dump()));

  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "a"), 0)->toolTip().isEmpty());
  EXPECT_EQ(tw->item(rowOfText(tw, 0, "b"), 0)->toolTip(), "other");
}

// The streaming-picker shape (e.g. the ROS2 topic table): every tick re-delivers
// rows plus an index-keyed selection computed in plugin order. Selection must
// land on the delivered-first row (a) even though the view shows c on top.
TEST(TableBinding, SelectedRowsRespectSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);
  deliverAbcAndSortDescending(&root, tw);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"a", "1"}, {"b", "2"}, {"c", "3"}}, {0}).dump()));

  EXPECT_TRUE(tw->item(rowOfText(tw, 0, "a"), 0)->isSelected());
  EXPECT_FALSE(tw->item(rowOfText(tw, 0, "b"), 0)->isSelected());
  EXPECT_FALSE(tw->item(rowOfText(tw, 0, "c"), 0)->isSelected());
}

namespace {

// Radio-column fixture: radio widgets occupy column 0, so the row key text
// lives in column 1 (a/b/c). radio_checked_row is a plugin-order index.
nlohmann::json radioTableData(int checked_row) {
  nlohmann::json d;
  d["tbl"]["headers"] = {"R", "Name"};
  d["tbl"]["rows"] = std::vector<std::vector<std::string>>{{"", "a"}, {"", "b"}, {"", "c"}};
  d["tbl"]["radio_column"] = 0;
  d["tbl"]["radio_checked_row"] = checked_row;
  return d;
}

}  // namespace

// radio_checked_row=0 refers to the row delivered as "a"; after a user sort on
// the name column the check must follow that row, not view row 0.
TEST(TableBinding, RadioApplyRespectsSortedView) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(radioTableData(0).dump()));
  ASSERT_EQ(tw->rowCount(), 3);
  tw->sortItems(1, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 1)->text(), "c");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(radioTableData(0).dump()));

  EXPECT_EQ(checkedRadioRow(tw, 0), rowOfText(tw, 1, "a"));
}

// A user sort with NO re-delivery: guards the Qt behavior the radio column
// leans on — cell widgets are anchored to persistent model indexes, so the
// checked radio travels with its row when the view re-sorts. If a Qt upgrade
// ever breaks this, the host would need to re-sync radios after layoutChanged.
TEST(TableBinding, RadioReChecksAfterUserSort) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(radioTableData(0).dump()));
  ASSERT_EQ(tw->rowCount(), 3);
  ASSERT_EQ(checkedRadioRow(tw, 0), 0);

  tw->sortItems(1, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 1)->text(), "c");

  EXPECT_EQ(checkedRadioRow(tw, 0), rowOfText(tw, 1, "a"));
}

// Clicking a radio must report the row's plugin index (its position in the
// delivered order), not its current view position.
TEST(TableBinding, RadioEmitTranslatesSortedViewRow) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  std::vector<std::pair<std::string, std::string>> events;
  PJ::connectWidgetSignals(
      &root, [&events](const std::string& name, const std::string& json) { events.push_back({name, json}); });

  PJ::applyWidgetData(&root, PJ::WidgetDataView(radioTableData(0).dump()));
  ASSERT_EQ(tw->rowCount(), 3);
  tw->sortItems(1, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 1)->text(), "c");

  // View row 0 is now the row delivered as "c" (plugin index 2).
  auto* radio = qobject_cast<QRadioButton*>(tw->cellWidget(0, 0));
  ASSERT_NE(radio, nullptr);
  radio->click();

  const std::optional<int> emitted = lastEmittedInt(events, "tbl", "table_radio_row");
  ASSERT_TRUE(emitted.has_value());
  EXPECT_EQ(*emitted, 2);
}

// Double-clicking a row must report its plugin index, not its view position —
// plugins resolve the index against the array they delivered rows from.
TEST(TableBinding, TableDoubleClickEmitsPluginIndex) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  std::vector<std::pair<std::string, std::string>> events;
  PJ::connectWidgetSignals(
      &root, [&events](const std::string& name, const std::string& json) { events.push_back({name, json}); });

  deliverAbcAndSortDescending(&root, tw);

  emit tw->cellDoubleClicked(0, 0);  // view row 0 = "c" = plugin index 2

  const std::optional<int> emitted = lastEmittedInt(events, "tbl", "item_double_clicked_index");
  ASSERT_TRUE(emitted.has_value());
  EXPECT_EQ(*emitted, 2);
}

// Same contract for QListWidget: a sorted list re-orders the view, but the
// double-click index the plugin receives must be the delivered-order position.
TEST(TableBinding, ListDoubleClickEmitsPluginIndex) {
  QWidget root;
  auto* lw = new QListWidget(&root);
  lw->setObjectName("lst");
  lw->setSortingEnabled(true);

  std::vector<std::pair<std::string, std::string>> events;
  PJ::connectWidgetSignals(
      &root, [&events](const std::string& name, const std::string& json) { events.push_back({name, json}); });

  nlohmann::json d;
  d["lst"]["list_items"] = {"c", "a", "b"};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));
  ASSERT_EQ(lw->count(), 3);
  ASSERT_EQ(lw->item(0)->text(), "a");  // list sorted itself on insert

  emit lw->itemDoubleClicked(lw->item(0));  // "a" was delivered at index 1

  const std::optional<int> emitted = lastEmittedInt(events, "lst", "item_double_clicked_index");
  ASSERT_TRUE(emitted.has_value());
  EXPECT_EQ(*emitted, 1);
}

// Two rows sharing a key text: the translation pairs duplicates positionally,
// which keeps set-level semantics exact — both "x" rows visible, "y" hidden.
TEST(TableBinding, DuplicateKeysStayConsistentUnderSort) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(tableData("tbl", {{"x", "1"}, {"x", "2"}, {"y", "3"}}, {}).dump()));
  ASSERT_EQ(tw->rowCount(), 3);
  tw->sortItems(1, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(1, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 1)->text(), "3");

  nlohmann::json d;
  d["tbl"]["visible_rows"] = {0, 1};  // both "x" rows, delivered first and second
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_FALSE(tw->isRowHidden(rowOfText(tw, 1, "1")));
  EXPECT_FALSE(tw->isRowHidden(rowOfText(tw, 1, "2")));
  EXPECT_TRUE(tw->isRowHidden(rowOfText(tw, 1, "3")));
}

namespace {

// Radio-column fixture with a THIRD, differentiating column: two rows share
// identical key-column ("Name") text but differ in "Type" — e.g. two
// same-named topics disambiguated by type. Duplicate-key translation must
// still resolve each row to its own distinct plugin index, not just any row
// with a matching key.
nlohmann::json duplicateKeyRadioTableData(int checked_row) {
  nlohmann::json d;
  d["tbl"]["headers"] = {"R", "Name", "Type"};
  d["tbl"]["rows"] = std::vector<std::vector<std::string>>{{"", "x", "typeA"}, {"", "x", "typeB"}};
  d["tbl"]["radio_column"] = 0;
  d["tbl"]["radio_checked_row"] = checked_row;
  return d;
}

}  // namespace

// radio_checked_row=0 refers to the row DELIVERED first ("x"/"typeA"), not any
// row whose key-column text happens to match "x" — "typeB" also reads "x" in
// the Name column, so a text-only lookup can't tell the two rows apart, and
// the checked radio must still follow the one the plugin actually meant.
TEST(TableBinding, RadioApplyDisambiguatesDuplicateKeysUnderSort) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  PJ::applyWidgetData(&root, PJ::WidgetDataView(duplicateKeyRadioTableData(0).dump()));
  ASSERT_EQ(tw->rowCount(), 2);
  // Sort descending on Type so the view now shows typeB above typeA — the
  // duplicate "x" rows have swapped view positions relative to delivery order.
  tw->sortItems(2, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(2, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 2)->text(), "typeB");

  PJ::applyWidgetData(&root, PJ::WidgetDataView(duplicateKeyRadioTableData(0).dump()));

  EXPECT_EQ(checkedRadioRow(tw, 0), rowOfText(tw, 2, "typeA"));
}

// Clicking a radio on a duplicate-key row must report ITS OWN plugin index,
// not the index of the other row sharing the same key-column text.
TEST(TableBinding, RadioEmitDisambiguatesDuplicateKeysUnderSort) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setSortingEnabled(true);

  std::vector<std::pair<std::string, std::string>> events;
  PJ::connectWidgetSignals(
      &root, [&events](const std::string& name, const std::string& json) { events.push_back({name, json}); });

  PJ::applyWidgetData(&root, PJ::WidgetDataView(duplicateKeyRadioTableData(0).dump()));
  ASSERT_EQ(tw->rowCount(), 2);
  tw->sortItems(2, Qt::DescendingOrder);
  tw->horizontalHeader()->setSortIndicator(2, Qt::DescendingOrder);
  ASSERT_EQ(tw->item(0, 2)->text(), "typeB");

  // View row 0 is "typeB" — plugin index 1 — even though its Name column ("x")
  // matches the other duplicate row too.
  auto* radio = qobject_cast<QRadioButton*>(tw->cellWidget(0, 0));
  ASSERT_NE(radio, nullptr);
  radio->click();

  const std::optional<int> emitted = lastEmittedInt(events, "tbl", "table_radio_row");
  ASSERT_TRUE(emitted.has_value());
  EXPECT_EQ(*emitted, 1);
}

// Tables whose rows never came from the plugin (predefined in a .ui, filled by
// hand) have no recorded plugin order — index-keyed state must keep meaning
// raw view rows there.
TEST(TableBinding, NoStoredOrderFallsBackToRawIndex) {
  QWidget root;
  auto* tw = new QTableWidget(&root);
  tw->setObjectName("tbl");
  tw->setColumnCount(2);
  tw->setRowCount(2);
  tw->setItem(0, 0, new QTableWidgetItem("a0"));
  tw->setItem(0, 1, new QTableWidgetItem("b0"));
  tw->setItem(1, 0, new QTableWidgetItem("a1"));
  tw->setItem(1, 1, new QTableWidgetItem("b1"));

  nlohmann::json d;
  d["tbl"]["selected_rows"] = {1};
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_FALSE(tw->item(0, 0)->isSelected());
  EXPECT_TRUE(tw->item(1, 0)->isSelected());
}

// ==========================================================================
// Code editor — caret offset round-trips via code_cursor
// ==========================================================================

TEST(CodeEditorBinding, AppliesCodeCursor) {
  QWidget root;
  auto* pte = new QPlainTextEdit(&root);
  pte->setObjectName("editor");

  nlohmann::json d;
  d["editor"]["code_content"] = "robot == bonirob";
  d["editor"]["code_cursor"] = 5;
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_EQ(pte->toPlainText().toStdString(), "robot == bonirob");
  EXPECT_EQ(pte->textCursor().position(), 5);
}

TEST(CodeEditorBinding, ClampsOutOfRangeCodeCursor) {
  QWidget root;
  auto* pte = new QPlainTextEdit(&root);
  pte->setObjectName("editor");

  nlohmann::json d;
  d["editor"]["code_content"] = "abc";
  d["editor"]["code_cursor"] = 999;
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  EXPECT_EQ(pte->textCursor().position(), 3);  // clamped to text length
}

// Opt-in (setCodeCaretTracking): a cursor move with no text edit emits a
// code_changed event carrying the new caret offset.
TEST(CodeEditorBinding, CaretTrackingEmitsOnCursorMove) {
  QWidget root;
  auto* pte = new QPlainTextEdit(&root);
  pte->setObjectName("editor");

  nlohmann::json d;
  d["editor"]["code_language"] = "lua";
  d["editor"]["code_content"] = "robot == x";
  d["editor"]["code_caret_tracking"] = true;
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  {
    QTextCursor tc = pte->textCursor();
    tc.setPosition(0);
    pte->setTextCursor(tc);
  }

  std::vector<std::string> events;
  PJ::connectWidgetSignals(&root, [&](const std::string& name, const std::string& json) {
    if (name == "editor") {
      events.push_back(json);
    }
  });

  QTextCursor tc = pte->textCursor();
  tc.setPosition(3);
  pte->setTextCursor(tc);  // cursor move only — no text change

  ASSERT_FALSE(events.empty());
  auto ev = nlohmann::json::parse(events.back());
  EXPECT_EQ(ev["code_cursor"], 3);
}

// Opt-out (the default): a cursor move emits nothing; a text edit still emits,
// but without a caret offset — the pre-caret behavior, so editors that only
// validate code aren't re-run on every cursor move.
TEST(CodeEditorBinding, NoCaretTrackingIgnoresCursorMoves) {
  QWidget root;
  auto* pte = new QPlainTextEdit(&root);
  pte->setObjectName("editor");

  nlohmann::json d;
  d["editor"]["code_language"] = "lua";
  d["editor"]["code_content"] = "robot == x";
  PJ::applyWidgetData(&root, PJ::WidgetDataView(d.dump()));

  {
    QTextCursor tc = pte->textCursor();
    tc.setPosition(0);
    pte->setTextCursor(tc);
  }

  std::vector<std::string> events;
  PJ::connectWidgetSignals(&root, [&](const std::string& name, const std::string& json) {
    if (name == "editor") {
      events.push_back(json);
    }
  });

  QTextCursor tc = pte->textCursor();
  tc.setPosition(3);
  pte->setTextCursor(tc);  // cursor move only
  EXPECT_TRUE(events.empty()) << "opt-out editor must not emit on cursor moves";

  pte->setPlainText("changed");  // a real edit still fires
  ASSERT_EQ(events.size(), 1u);
  auto ev = nlohmann::json::parse(events.back());
  EXPECT_TRUE(ev.contains("code_changed"));
  EXPECT_FALSE(ev.contains("code_cursor")) << "opt-out editor carries no caret offset";
}

// ==========================================================================
// Named icon resolver
// ==========================================================================

TEST(NamedIconResolver, MapsKnownIdsToThemedResourcePaths) {
  EXPECT_EQ(PJ::resolveNamedIconPath("link"), u":/resources/svg/link.svg"_s);
  EXPECT_EQ(PJ::resolveNamedIconPath("contract"), u":/resources/svg/contract.svg"_s);
  EXPECT_EQ(PJ::resolveNamedIconPath("plug_connect"), u":/resources/svg/plug_connect.svg"_s);
  EXPECT_EQ(PJ::resolveNamedIconPath("refresh"), u":/resources/svg/refresh.svg"_s);
  EXPECT_EQ(PJ::resolveNamedIconPath("add"), u":/resources/svg/add.svg"_s);
}

TEST(NamedIconResolver, ReturnsEmptyForUnknownOrEmptyId) {
  EXPECT_TRUE(PJ::resolveNamedIconPath("does-not-exist").isEmpty());
  EXPECT_TRUE(PJ::resolveNamedIconPath("").isEmpty());
  // Case-sensitive: ids are exact semantic tokens, not free text.
  EXPECT_TRUE(PJ::resolveNamedIconPath("Link").isEmpty());
}

// ==========================================================================
// Byte-identical payload skip (parity with PanelEngine's applyAndDiff guard)
// ==========================================================================

namespace identical_mock {

// Chatty-idle plugin: every tick reports "re-read me" while widget_data stays
// byte-identical — the exact traffic the skip guard exists to absorb. After
// `accept_after_ticks` ticks (when >= 0) the payload gains __request_accept
// and stays byte-stable, so the request's first delivery must be honored —
// there is no identical-bytes retry to fall back on.
struct Ctx {
  std::string manifest = R"({"name":"identical"})";
  std::string ui = R"(<ui version="4.0"><class>D</class><widget class="QWidget" name="D">
<layout class="QVBoxLayout"><item><widget class="QLineEdit" name="name_input"/></item></layout>
</widget></ui>)";
  std::string payload = R"({"name_input":{"text":"fixed"}})";
  int accept_after_ticks = -1;
  int ticks = 0;
};

// Knob consumed by create(): ticks after which the payload raises
// __request_accept (-1 = never). Set before constructing the DialogHandle.
int& acceptAfterTicksKnob() {
  static int v = -1;
  return v;
}

void* create() noexcept {
  auto* c = new Ctx();
  c->accept_after_ticks = acceptAfterTicksKnob();
  return c;
}
void destroy(void* ctx) noexcept {
  delete static_cast<Ctx*>(ctx);
}
const char* manifest(void* ctx) noexcept {
  return static_cast<Ctx*>(ctx)->manifest.c_str();
}
const char* uiContent(void* ctx) noexcept {
  return static_cast<Ctx*>(ctx)->ui.c_str();
}
const char* widgetData(void* ctx) noexcept {
  auto* c = static_cast<Ctx*>(ctx);
  if (c->accept_after_ticks >= 0 && c->ticks >= c->accept_after_ticks) {
    c->payload = R"({"name_input":{"text":"fixed"},"__request_accept":true})";
  }
  return c->payload.c_str();
}
bool widgetEvent(void*, const char*, const char*, PJ_error_t*) noexcept {
  return false;
}
bool tick(void* ctx, PJ_error_t*) noexcept {
  ++static_cast<Ctx*>(ctx)->ticks;
  return true;
}
void accepted(void*, const char*) noexcept {}
void rejected(void*) noexcept {}
bool saveConfig(void*, PJ_string_view_t* out_json, PJ_error_t*) noexcept {
  out_json->data = "";
  out_json->size = 0;
  return true;
}
bool loadConfig(void*, PJ_string_view_t, PJ_error_t*) noexcept {
  return true;
}

const PJ_dialog_vtable_t* vtable() {
  static const PJ_dialog_vtable_t vt = [] {
    PJ_dialog_vtable_t v{};
    v.protocol_version = PJ_DIALOG_PROTOCOL_VERSION;
    v.struct_size = sizeof(PJ_dialog_vtable_t);
    v.create = create;
    v.destroy = destroy;
    v.get_manifest = manifest;
    v.get_ui_content = uiContent;
    v.get_widget_data = widgetData;
    v.on_widget_event = widgetEvent;
    v.on_tick = tick;
    v.on_accepted = accepted;
    v.on_rejected = rejected;
    v.save_config = saveConfig;
    v.load_config = loadConfig;
    return v;
  }();
  return &vt;
}

}  // namespace identical_mock

TEST_F(DialogEngineTest, IdenticalPayloadTicksAreSkipped) {
  identical_mock::acceptAfterTicksKnob() = -1;
  PJ::DialogHandle handle(identical_mock::vtable());
  PJ::DialogEngineConfig config;
  config.tick_interval_ms = 1;
  PJ::DialogEngine engine(std::move(handle), config);

  // Close the modal from inside its own event loop, but only once enough ticks
  // ran — gating on the live stats keeps this immune to slow-runner startup.
  QTimer close_timer;
  close_timer.setInterval(20);
  QObject::connect(&close_timer, &QTimer::timeout, [&engine] {
    if (engine.lastStats().tick_count < 3) {
      return;
    }
    for (QWidget* w : QApplication::topLevelWidgets()) {
      if (auto* dlg = qobject_cast<QDialog*>(w); dlg != nullptr && dlg->isVisible()) {
        dlg->reject();
      }
    }
  });
  close_timer.start();
  (void)engine.showDialog();

  const auto stats = engine.lastStats();
  ASSERT_GE(stats.tick_count, 2);
  // Byte-identical re-emissions never reach the parser or the widgets.
  EXPECT_EQ(stats.diff_apply_count, 0);
  EXPECT_GE(stats.skipped_identical_count, stats.tick_count - 1);
}

// The first delivery of a payload carrying __request_accept must be honored on
// the tick path itself: once the bytes go stable, the skip guard removes the
// identical-bytes retry that could previously pick the command up later.
TEST_F(DialogEngineTest, TickHonorsRequestAccept) {
  identical_mock::acceptAfterTicksKnob() = 2;
  PJ::DialogHandle handle(identical_mock::vtable());
  identical_mock::acceptAfterTicksKnob() = -1;
  PJ::DialogEngineConfig config;
  config.tick_interval_ms = 1;
  PJ::DialogEngine engine(std::move(handle), config);

  // Safety net: reject if the accept never happens, so the test cannot hang.
  QTimer bailout_timer;
  bailout_timer.setInterval(2000);
  QObject::connect(&bailout_timer, &QTimer::timeout, [] {
    for (QWidget* w : QApplication::topLevelWidgets()) {
      if (auto* dlg = qobject_cast<QDialog*>(w); dlg != nullptr && dlg->isVisible()) {
        dlg->reject();
      }
    }
  });
  bailout_timer.start();

  EXPECT_EQ(engine.showDialog(), PJ::DialogResult::kAccepted);
}

// ==========================================================================
// main with QApplication
// ==========================================================================

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
