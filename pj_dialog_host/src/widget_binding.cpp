// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_plotting/PlotWidget.h>
#include <pj_runtime/AppSession.h>
#include <pj_runtime/CatalogModel.h>
#include <pj_widgets/DateRangePicker.h>
#include <pj_widgets/RangeSlider.h>
#include <pj_widgets/SvgUtil.h>
#include <pj_widgets/ToggleSwitch.h>
#include <qwt_plot_curve.h>
#include <qwt_point_data.h>

#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <set>

#include "lua_syntax_highlighter.hpp"
#include "python_syntax_highlighter.hpp"
using namespace Qt::StringLiterals;

namespace PJ {

QString resolveNamedIconPath(std::string_view icon_name) {
  if (icon_name == "link") {
    return u":/resources/svg/link.svg"_s;
  }
  if (icon_name == "contract") {
    return u":/resources/svg/contract.svg"_s;
  }
  if (icon_name == "plug_connect") {
    return u":/resources/svg/plug_connect.svg"_s;
  }
  if (icon_name == "refresh") {
    return u":/resources/svg/refresh.svg"_s;
  }
  if (icon_name == "search") {
    return u":/resources/svg/search_light.svg"_s;
  }
  if (icon_name == "add") {
    return u":/resources/svg/add.svg"_s;
  }
  return {};
}

namespace {

// Human-readable nanosecond duration: "42s", "12m 30s", "3h 45m", "2d 5h 30m".
std::string formatDuration(std::int64_t duration_ns) {
  const std::int64_t total_secs = duration_ns / 1'000'000'000LL;
  if (total_secs < 60) {
    return std::to_string(total_secs) + "s";
  }
  const std::int64_t days = total_secs / 86400;
  const std::int64_t hours = (total_secs % 86400) / 3600;
  const std::int64_t minutes = (total_secs % 3600) / 60;
  const std::int64_t secs = total_secs % 60;
  if (days > 0) {
    return std::to_string(days) + "d " + std::to_string(hours) + "h " + std::to_string(minutes) + "m";
  }
  if (hours > 0) {
    return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
  }
  return std::to_string(minutes) + "m " + std::to_string(secs) + "s";
}

// Map a slider position in [0, slider_max] onto absolute nanoseconds within
// [min_ns, max_ns].
std::int64_t sliderToNs(int pos, int slider_max, std::int64_t min_ns, std::int64_t max_ns) {
  if (slider_max <= 0) {
    return min_ns;
  }
  const double fraction = static_cast<double>(pos) / static_cast<double>(slider_max);
  return min_ns + static_cast<std::int64_t>(fraction * static_cast<double>(max_ns - min_ns));
}

}  // namespace

// ---------------------------------------------------------------------------
// apply_widget_data — push WidgetDataView values into Qt widgets
// ---------------------------------------------------------------------------

// Item-data role tagging a QTableWidgetItem/QListWidgetItem with the plugin
// row/list index it was written for. Anchored to the item object itself — Qt
// relocates item pointers (not their data) when it re-sorts — so the
// row-translation functions below recover the true originating index
// directly, with no key-text matching and no ambiguity when two rows/items
// share identical text.
constexpr int kPluginRowRole = Qt::UserRole + 1;

// Push `rows` into the table with minimal churn. All table aspects
// (rows/selection/visibility) share one widget-data key, so every selection
// change and every streamed per-row detail update re-delivers the whole rows
// array. When the shape (row + column count) is unchanged — the common case —
// only the cells whose text actually differs are updated in place: this keeps
// the existing QTableWidgetItems (so selection + scroll survive), avoids the
// ResizeToContents re-measure a full rebuild triggers, and lets streamed detail
// fill in cell-by-cell instead of snapping in all at once. Only a row/column
// count change forces a full rebuild.
static void applyTableRows(QTableWidget* tw, const std::vector<std::vector<std::string>>& rows) {
  // Rows arrive in the plugin's own order and are written by model-row index. With
  // sorting enabled QTableWidget physically re-sorts the model on every setItem/
  // setText, so a mid-loop re-sort remaps the indices and the remaining writes land
  // on the wrong rows (blank cells, name↔value pairs scrambled, duplicated rows).
  // Suspend sorting before the first cell write and restore it once at the end, so
  // Qt applies a single clean sort. Suspending lazily means a streaming re-delivery
  // that changes no cell never toggles sorting and pays no re-sort — preserving the
  // same-shape path's minimal-churn intent. Text-keyed selection restore
  // (selected_items) runs later, once the sort has settled, and still matches rows.
  const bool was_sorting = tw->isSortingEnabled();
  bool suspended = false;
  auto suspend_sorting = [&] {
    if (was_sorting && !suspended) {
      tw->setSortingEnabled(false);
      suspended = true;
    }
  };

  const bool same_shape = static_cast<std::size_t>(tw->rowCount()) == rows.size() &&
                          (rows.empty() || static_cast<std::size_t>(tw->columnCount()) == rows.front().size());
  if (same_shape) {
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto& row = rows[r];
      for (std::size_t c = 0; c < row.size(); ++c) {
        const QString text = QString::fromStdString(row[c]);
        QTableWidgetItem* item = tw->item(static_cast<int>(r), static_cast<int>(c));
        if (item == nullptr) {
          suspend_sorting();
          item = new QTableWidgetItem(text);
          tw->setItem(static_cast<int>(r), static_cast<int>(c), item);
        } else if (item->text() != text) {
          suspend_sorting();
          item->setText(text);
        }
        item->setData(kPluginRowRole, static_cast<int>(r));
      }
    }
  } else {
    suspend_sorting();
    const bool updates = tw->updatesEnabled();
    tw->setUpdatesEnabled(false);
    tw->setRowCount(static_cast<int>(rows.size()));
    for (std::size_t r = 0; r < rows.size(); ++r) {
      const auto& row = rows[r];
      for (std::size_t c = 0; c < row.size(); ++c) {
        auto* item = new QTableWidgetItem(QString::fromStdString(row[c]));
        item->setData(kPluginRowRole, static_cast<int>(r));
        tw->setItem(static_cast<int>(r), static_cast<int>(c), item);
      }
    }
    tw->setUpdatesEnabled(updates);
  }

  if (suspended) {
    tw->setSortingEnabled(true);
  }
}

namespace {
// Bridges radio-cell clicks back to the dialog event stream. connectWidgetSignals
// owns the event callback but the radio cells don't exist yet then (rows arrive
// later via applyWidgetData), so it stashes this holder on the table and
// applyTableRadioColumn wires each radio to it as rows materialise. Found by a
// fixed objectName + static_cast (no Q_OBJECT / moc needed in this TU).
class RadioEmitHolder : public QObject {
 public:
  RadioEmitHolder(QObject* parent, std::function<void(int)> fn) : QObject(parent), emit_row(std::move(fn)) {
    setObjectName(u"pj_radio_emit_holder"_s);
  }
  std::function<void(int)> emit_row;
};
}  // namespace

// Render `col` of `tw` as an exclusive radio group (one QRadioButton per row),
// check `checked_row`, and wire each radio's click to emit_row(its current row).
// Idempotent: reuses existing radios and only syncs the checked state, so it can
// run on every data apply without flicker. Rows that shrink away have their cell
// widgets destroyed by QTableWidget::setRowCount, which auto-removes them from the
// QButtonGroup. `clicked` fires on user interaction only (NOT on setChecked), so
// the checked-state sync below never feeds back as a spurious event.
static void applyTableRadioColumn(
    QTableWidget* tw, int col, int checked_row, const std::function<void(int)>& emit_row) {
  if (col < 0 || col >= tw->columnCount()) {
    return;
  }
  auto* group = tw->findChild<QButtonGroup*>(u"pj_radio_group"_s, Qt::FindDirectChildrenOnly);
  if (group == nullptr) {
    group = new QButtonGroup(tw);
    group->setObjectName(u"pj_radio_group"_s);
    group->setExclusive(true);
  }
  for (int r = 0; r < tw->rowCount(); ++r) {
    auto* radio = qobject_cast<QRadioButton*>(tw->cellWidget(r, col));
    if (radio == nullptr) {
      radio = new QRadioButton(tw);
      radio->setStyleSheet(u"QRadioButton { margin-left: 8px; }"_s);
      tw->setCellWidget(r, col, radio);
      group->addButton(radio);
      // Resolve the row at click time: rows renumber as the user adds/removes
      // series, so a row index captured at creation would go stale.
      QObject::connect(radio, &QRadioButton::clicked, radio, [emit_row, tw, col, radio]() {
        for (int rr = 0; rr < tw->rowCount(); ++rr) {
          if (tw->cellWidget(rr, col) == radio) {
            emit_row(rr);
            return;
          }
        }
      });
    }
    radio->setChecked(r == checked_row);
  }

  // Keep the radio column just wide enough for the button, and stretch the first
  // non-radio column instead. installTreeLikeHeader stretches column 0 by default,
  // which would over-widen the radio when it is the first column.
  auto* header = tw->horizontalHeader();
  header->setSectionResizeMode(col, QHeaderView::Fixed);
  tw->setColumnWidth(col, 36);
  for (int c = 0; c < tw->columnCount(); ++c) {
    if (c != col) {
      header->setSectionResizeMode(c, QHeaderView::Stretch);
      break;
    }
  }
}

// Key text identifying a table row for text-keyed selection (selected_items
// apply + selection-changed emit). Plugin-fed tables read the key column
// recorded at delivery time (_pj_plugin_key_col, see recordPluginKeyColumn);
// it is authoritative even before radio cell widgets materialise. Tables
// never fed by a delivery (predefined in a .ui) fall back to scanning for the
// first column that hosts no cell widget — columns with one (e.g. an
// exclusive radio column) carry no selectable item text. The scan stops at
// that column even if it has no item (nullopt), rather than falling through
// to a later column.
static std::optional<std::string> tableRowKeyText(const QTableWidget* tw, int row) {
  const QVariant recorded_col = tw->property("_pj_plugin_key_col");
  if (recorded_col.isValid()) {
    if (auto* item = tw->item(row, recorded_col.toInt())) {
      return item->text().toStdString();
    }
    return std::nullopt;
  }
  for (int c = 0; c < tw->columnCount(); ++c) {
    if (tw->cellWidget(row, c) == nullptr) {
      if (auto* item = tw->item(row, c)) {
        return item->text().toStdString();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Plugin-order ↔ view-order row translation.
//
// Plugins deliver `rows` in their own order and key every index-based aspect
// (selected_rows, visible_rows, disabled_rows, radio_checked_row) — and
// interpret every emitted index (table_radio_row, item_double_clicked_index) —
// against that order. With sortingEnabled the user can re-order the view, so
// the host translates row indices in both directions. Rows are identified by
// the kPluginRowRole tag applyTableRows() stamps on every item, not by text,
// so two rows sharing identical key-column text still resolve to their own
// distinct plugin index (a text-keyed lookup could not tell them apart).
// Tables whose rows never came from a plugin delivery (predefined in a .ui)
// have no tagged items and keep raw-index semantics via the identity fallback.
// ---------------------------------------------------------------------------

// Record the key column for this delivery. `radio_col` is the column rendered
// as radio widgets this delivery (or -1): it carries no item text, so the key
// column is the first column other than it. Consumed by tableRowKeyText,
// which needs the key column even before radio cell widgets materialise; row
// index translation does not need this (see viewToPluginRowMap).
static void recordPluginKeyColumn(QTableWidget* tw, int radio_col) {
  tw->setProperty("_pj_plugin_key_col", radio_col == 0 ? 1 : 0);
}

// view row -> plugin row for every current row, read directly off each row's
// kPluginRowRole item tag. Falls back to identity (raw row index) for a row
// whose items were never tagged — normal for a .ui-predefined table never fed
// by a plugin delivery.
static std::vector<int> viewToPluginRowMap(const QTableWidget* tw) {
  std::vector<int> map(static_cast<std::size_t>(tw->rowCount()));
  for (int r = 0; r < tw->rowCount(); ++r) {
    int plugin_row = r;
    for (int c = 0; c < tw->columnCount(); ++c) {
      if (const QTableWidgetItem* item = tw->item(r, c)) {
        const QVariant tag = item->data(kPluginRowRole);
        if (tag.isValid()) {
          plugin_row = tag.toInt();
        }
        break;
      }
    }
    map[static_cast<std::size_t>(r)] = plugin_row;
  }
  return map;
}

// Inverse of a viewToPluginRowMap result: plugin row -> current view row.
static std::vector<int> invertRowMap(const std::vector<int>& view_to_plugin) {
  std::vector<int> inverse(view_to_plugin.size());
  for (std::size_t r = 0; r < view_to_plugin.size(); ++r) {
    inverse[static_cast<std::size_t>(view_to_plugin[r])] = static_cast<int>(r);
  }
  return inverse;
}

// Emit-direction translation for a single row (radio click, double-click).
static int viewRowToPluginRow(const QTableWidget* tw, int view_row) {
  const std::vector<int> map = viewToPluginRowMap(tw);
  if (view_row < 0 || static_cast<std::size_t>(view_row) >= map.size()) {
    return view_row;
  }
  return map[static_cast<std::size_t>(view_row)];
}

// Plugin index of a list item: the kPluginRowRole tag stamped on it when
// listItems was applied — anchored to the item itself, so it survives list
// sorting and needs no text matching (safe even with duplicate item text).
// Falls back to the raw view row for lists never fed by a delivery.
static int listItemPluginIndex(const QListWidget* lw, const QListWidgetItem* item) {
  const QVariant tag = item->data(kPluginRowRole);
  return tag.isValid() ? tag.toInt() : lw->row(item);
}

// True when `tw`'s header labels already equal `headers`.
static bool tableMatchesHeaders(const QTableWidget* tw, const QStringList& headers) {
  if (tw->columnCount() != headers.size()) {
    return false;
  }
  for (int i = 0; i < headers.size(); ++i) {
    const QTableWidgetItem* h = tw->horizontalHeaderItem(i);
    if (h == nullptr || h->text() != headers[i]) {
      return false;
    }
  }
  return true;
}

// Size a topic/curve table the way it reads best: the first column stretches to
// fill the viewport (no dead grey space to the right) while every other column is
// a fixed, user-draggable width. WA_Hover lets the QSS `QHeaderView::section:hover`
// divider tint fire; header weight is left to the app stylesheet (the global
// `QHeaderView::section { font-weight: normal }` rule), which reads consistently
// with CurveTreeView — a widget-side setFont would be ignored while a stylesheet
// is active anyway.
//
// The resize modes are persistent (set once and kept), so this is guarded by a
// dynamic property and a column-count check: it's safe to call on every
// widget_data delivery — it configures the first time the table actually has
// columns and no-ops after. This is deliberately NOT gated on the header *labels*
// changing: dialogs whose .ui predefines column headers (e.g. MCAP's tableWidget)
// match the plugin's setTableHeaders() verbatim, so a label-change gate would skip
// them entirely and leave the .ui's default un-stretched sizing — the very bug
// this fixes.
static void installTreeLikeHeader(QTableWidget* tw) {
  auto* header = tw->horizontalHeader();
  if (header->count() == 0 || tw->property("pjTreeLikeHeader").toBool()) {
    return;
  }
  tw->setProperty("pjTreeLikeHeader", true);

  header->setStretchLastSection(false);
  header->setMinimumSectionSize(20);
  header->setAttribute(Qt::WA_Hover, true);
  header->viewport()->setAttribute(Qt::WA_Hover, true);

  // The first (name) column stretches to fill the viewport: long names aren't
  // clipped and no dead space trails the last column, with no dependence on the
  // viewport already being laid out. The data columns are Interactive so their
  // dividers DRAG to resize (Stretch / ResizeToContents are auto-sized and can't be
  // dragged — the reason the separators looked dead); resizing a data column gives
  // and takes from the stretched name column.
  header->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int i = 1; i < header->count(); ++i) {
    header->setSectionResizeMode(i, QHeaderView::Interactive);
    header->resizeSection(i, 96);
  }
}

static void applyToWidget(
    QWidget* w, std::string_view name, const PJ::WidgetDataView& view, PJ::AppSession* session = nullptr,
    PJ::CatalogModel* catalog = nullptr) {
  const QSignalBlocker blocker(w);

  // --- Generic properties (any widget) ---
  // For a widget whose plain control was swapped for a styled replacement (see
  // widget_adapters), `enabled` is written straight to the hidden original and
  // reaches the replacement via syncStyledWidget below. `visible` is redirected
  // onto the original as a desired-visible the replacement derives from (e.g. a
  // DualOptionsWidget's visibility is the OR of its two hidden radios), so it
  // goes through redirectAdaptedVisibility; an un-adapted widget just sets its
  // own visibility.
  if (auto v = view.enabled(name)) {
    w->setEnabled(*v);
  }
  if (auto v = view.visible(name)) {
    if (!redirectAdaptedVisibility(w, *v)) {
      w->setVisible(*v);
    }
  }

  // --- Generic field-validity indicator (any widget) ---
  // The plugin owns the validation rule and pushes {valid, tooltip}; the host
  // renders a soft cue (the tooltip plus a light-red background on the field
  // itself when invalid) without needing a per-field indicator widget. The cue
  // is scoped by objectName so child widgets are unaffected; cleared when valid.
  // PJ3 parity: invalid input fields use a #ffcccc background, not a border.
  if (auto ok = view.fieldValid(name)) {
    if (auto tip = view.fieldValidTooltip(name)) {
      w->setToolTip(QString::fromStdString(*tip));
    }
    const QString sel = w->objectName().isEmpty() ? QString() : u"#%1"_s.arg(w->objectName());
    w->setStyleSheet(*ok || sel.isEmpty() ? QString() : sel + u" { background-color: #ffcccc; }"_s);
  }

  // --- QLineEdit ---
  if (auto* le = qobject_cast<QLineEdit*>(w)) {
    if (auto v = view.text(name)) {
      le->setText(QString::fromStdString(*v));
    }
    if (auto v = view.placeholder(name)) {
      le->setPlaceholderText(QString::fromStdString(*v));
    }
    if (auto v = view.readOnly(name)) {
      le->setReadOnly(*v);
    }
    return;
  }

  // --- QPlainTextEdit ---
  if (auto* pte = qobject_cast<QPlainTextEdit*>(w)) {
    if (auto code = view.codeContent(name)) {
      // Code editors use a light-theme syntax highlighter (dark-on-white token
      // colors), so the text area must stay white regardless of the app's
      // light/dark theme. Tag the widget so the global stylesheet paints it
      // white; set once + re-polish so the attribute selector re-evaluates.
      if (!pte->property("_pj_code_editor").toBool()) {
        pte->setProperty("_pj_code_editor", true);
        pte->style()->unpolish(pte);
        pte->style()->polish(pte);
      }
      // Also tag the editor's immediate container pane so the area around the
      // editor (assist dropdowns, labels) shares the white code surface in the
      // light theme. WA_StyledBackground lets the QSS background paint on a
      // plain QWidget; the dark theme leaves the pane at its normal background.
      if (auto* pane = pte->parentWidget(); pane != nullptr && !pane->property("_pj_code_editor_pane").toBool()) {
        pane->setProperty("_pj_code_editor_pane", true);
        pane->setAttribute(Qt::WA_StyledBackground, true);
        pane->style()->unpolish(pane);
        pane->style()->polish(pane);
      }
      // Code editor mode: only update if content actually differs (preserve cursor).
      QString new_text = QString::fromStdString(*code);
      if (pte->toPlainText() != new_text) {
        pte->setPlainText(new_text);
      }
      // Install or swap syntax highlighter when the language changes.
      if (auto lang = view.codeLanguage(name)) {
        QString current = pte->property("_pj_code_lang").toString();
        QString requested = QString::fromStdString(*lang);
        if (current != requested) {
          pte->setProperty("_pj_code_lang", requested);
          if (auto* old = pte->document()->findChild<QSyntaxHighlighter*>()) {
            delete old;
          }
          if (*lang == "lua") {
            new PJ::LuaSyntaxHighlighter(pte->document());
          } else if (*lang == "python") {
            new PJ::PythonSyntaxHighlighter(pte->document());
          }
        }
      }
      // Place the caret where the plugin asked (e.g. just past an inserted
      // completion), now that the new text is in place.
      if (auto cur = view.codeCursor(name)) {
        QTextCursor tc = pte->textCursor();
        tc.setPosition(qBound(0, *cur, static_cast<int>(pte->toPlainText().size())));
        pte->setTextCursor(tc);
      }
    } else if (auto pt = view.plainText(name)) {
      pte->setPlainText(QString::fromStdString(*pt));
    }
    if (auto v = view.readOnly(name)) {
      pte->setReadOnly(*v);
    }
    // Opt-in caret tracking: when set, connectWidgetSignals also wires
    // cursorPositionChanged so the plugin sees caret moves, not just edits.
    if (auto track = view.codeCaretTracking(name)) {
      pte->setProperty("_pj_caret_tracking", *track);
    }
    return;
  }

  // --- QComboBox ---
  if (auto* cb = qobject_cast<QComboBox*>(w)) {
    if (auto v = view.items(name)) {
      // Rebuild only when the item set actually changed. For an editable combo
      // (where text + items share one diff key) an unconditional clear()+add
      // would wipe the line edit and bounce the caret to the end on every
      // keystroke tick. Comparing first keeps mid-text editing stable.
      QStringList incoming;
      incoming.reserve(static_cast<qsizetype>(v->size()));
      for (const auto& item : *v) {
        incoming << QString::fromStdString(item);
      }
      QStringList current;
      current.reserve(cb->count());
      for (int i = 0; i < cb->count(); ++i) {
        current << cb->itemText(i);
      }
      if (current != incoming) {
        const QString saved = cb->isEditable() ? cb->currentText() : QString();
        cb->clear();
        cb->addItems(incoming);
        if (cb->isEditable()) {
          cb->setCurrentText(saved);
        }
      }
    }
    if (auto v = view.currentIndex(name)) {
      cb->setCurrentIndex(*v);
    }
    // Editable combos carry free text (e.g. a server URI). Reflect the
    // plugin's text() back into the line edit, guarded so a value identical
    // to what the user just typed doesn't reset the cursor mid-edit.
    if (cb->isEditable()) {
      if (auto v = view.text(name)) {
        const QString t = QString::fromStdString(*v);
        if (cb->currentText() != t) {
          cb->setCurrentText(t);
        }
      }
    }
    return;
  }

  // --- QCheckBox ---
  if (auto* ck = qobject_cast<QCheckBox*>(w)) {
    if (auto v = view.checked(name)) {
      ck->setChecked(*v);
    }
    if (auto v = view.text(name)) {
      ck->setText(QString::fromStdString(*v));
    }
    // Keep any styled replacement in sync, and adapt now if this checkbox just
    // became adaptable (e.g. its text arrived via data). Both no-op otherwise.
    syncStyledWidget(ck);
    tryAdaptStyledWidget(ck);
    return;
  }

  // --- ToggleSwitch (iOS-style toggle; QWidget, not QCheckBox) ---
  if (auto* ts = qobject_cast<ToggleSwitch*>(w)) {
    if (auto v = view.checked(name)) {
      ts->setChecked(*v, /*animate=*/false);  // no animation on programmatic sync
    }
    return;
  }

  // --- QRadioButton ---
  if (auto* rb = qobject_cast<QRadioButton*>(w)) {
    if (auto v = view.checked(name)) {
      rb->setChecked(*v);
    }
    // Keep any styled replacement in sync, and adapt the group now if it has
    // just become adaptable (e.g. data selected one option of a previously
    // unselected pair). Both no-op for un-adapted/non-adaptable widgets.
    syncStyledWidget(rb);
    tryAdaptStyledWidget(rb);
    return;
  }

  // --- QSpinBox ---
  if (auto* sb = qobject_cast<QSpinBox*>(w)) {
    if (auto v = view.rangeMin(name)) {
      sb->setMinimum(*v);
    }
    if (auto v = view.rangeMax(name)) {
      sb->setMaximum(*v);
    }
    if (auto v = view.valueInt(name)) {
      sb->setValue(*v);
    }
    return;
  }

  // --- QDoubleSpinBox ---
  if (auto* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
    if (auto v = view.valueDouble(name)) {
      dsb->setValue(*v);
    }
    return;
  }

  // --- QListWidget ---
  if (auto* lw = qobject_cast<QListWidget*>(w)) {
    if (auto v = view.listItems(name)) {
      lw->clear();
      for (std::size_t i = 0; i < v->size(); ++i) {
        auto* item = new QListWidgetItem(QString::fromStdString((*v)[i]));
        // Delivered index, so itemDoubleClicked can report the plugin's index
        // even when list sorting re-orders the view (see listItemPluginIndex).
        item->setData(kPluginRowRole, static_cast<int>(i));
        lw->addItem(item);
      }
    }
    if (auto v = view.selectedItems(name)) {
      std::set<std::string> selected(v->begin(), v->end());
      for (int i = 0; i < lw->count(); ++i) {
        auto* item = lw->item(i);
        item->setSelected(selected.count(item->text().toStdString()) > 0);
      }
    }
    return;
  }

  // --- QTableWidget ---
  if (auto* tw = qobject_cast<QTableWidget*>(w)) {
    // The dialog protocol carries no cell-edit event (only selection, double-click
    // and radio), so an edited cell can never be read back by the plugin — an
    // editable cell silently discards the edit on accept. Force read-only on every
    // protocol table so no picker looks editable when it isn't. Persistent and
    // idempotent, so setting it on each delivery is free. A future editable table
    // would need a new protocol event anyway, and would opt out here then.
    tw->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (auto v = view.tableHeaders(name)) {
      QStringList hdr;
      for (const auto& h : *v) {
        hdr << QString::fromStdString(h);
      }
      // Re-setting labels reconfigures the header (not free), so only do it when
      // they actually changed. The sizing setup below is separate: it must also
      // run for dialogs whose .ui predefines matching headers (e.g. MCAP), where
      // this branch is skipped — hence InstallTreeLikeHeader lives outside it.
      if (!tableMatchesHeaders(tw, hdr)) {
        tw->setColumnCount(static_cast<int>(hdr.size()));
        tw->setHorizontalHeaderLabels(hdr);
      }
      // First column fills the width, the rest hug content. Idempotent + guarded,
      // so calling it on every delivery is cheap (port/fix of #90).
      installTreeLikeHeader(tw);
    }
    // The radio column is read up front: recordPluginKeyColumn needs to know
    // which column carries radio widgets (no item text) to pick the key column.
    const std::optional<int> radio_col = view.tableRadioColumn(name);
    if (auto v = view.tableRows(name)) {
      applyTableRows(tw, *v);
      recordPluginKeyColumn(tw, radio_col.value_or(-1));
    }
    // Every index-keyed aspect below arrives in plugin row order; translate it
    // to the current (possibly user-sorted) view order. The maps reflect the
    // rows just applied above and are only built when this delivery actually
    // carries an index-keyed aspect — a rows-only streaming tick skips them.
    const auto visible_rows = view.visibleRows(name);
    const auto disabled_rows = view.disabledRows(name);
    const auto selected_rows = view.selectedRows(name);
    const auto cell_tooltips = view.cellTooltips(name);
    std::vector<int> view_to_plugin;
    std::vector<int> plugin_to_view;
    if (radio_col || visible_rows || disabled_rows || selected_rows || cell_tooltips) {
      view_to_plugin = viewToPluginRowMap(tw);
      plugin_to_view = invertRowMap(view_to_plugin);
    }
    // Radio column: render the designated column as an exclusive radio group and
    // sync the checked row. Build the radios UNCONDITIONALLY — a Modify flow delivers
    // pre-populated rows on the very first apply, which runs BEFORE connectWidgetSignals
    // stashes the holder; gating on the holder there left the radio column empty and
    // its width mis-stretched (unlike Create, whose rows arrive by drop after wiring).
    // The click callback resolves the holder lazily, so clicks still emit once it lands.
    if (radio_col) {
      const int checked_plugin_row = view.tableRadioCheckedRow(name).value_or(-1);
      const int checked_view_row =
          checked_plugin_row >= 0 && static_cast<std::size_t>(checked_plugin_row) < plugin_to_view.size()
              ? plugin_to_view[static_cast<std::size_t>(checked_plugin_row)]
              : -1;
      applyTableRadioColumn(tw, *radio_col, checked_view_row, [tw](int row) {
        if (auto* holder = static_cast<RadioEmitHolder*>(
                tw->findChild<QObject*>(u"pj_radio_emit_holder"_s, Qt::FindDirectChildrenOnly))) {
          holder->emit_row(row);
        }
      });
    }
    // Row visibility (live filtering): hide rows not in the visible set. Absent
    // (clearVisibleRows ⇒ nullopt) means "no change"; an empty set hides all.
    if (visible_rows) {
      std::set<int> visible(visible_rows->begin(), visible_rows->end());
      for (int r = 0; r < tw->rowCount(); ++r) {
        tw->setRowHidden(r, !visible.contains(view_to_plugin[static_cast<std::size_t>(r)]));
      }
    }
    if (disabled_rows) {
      std::set<int> disabled(disabled_rows->begin(), disabled_rows->end());
      for (int r = 0; r < tw->rowCount(); ++r) {
        bool is_disabled = disabled.count(view_to_plugin[static_cast<std::size_t>(r)]) > 0;
        for (int c = 0; c < tw->columnCount(); ++c) {
          if (auto* item = tw->item(r, c)) {
            auto flags = item->flags();
            if (is_disabled) {
              flags &= ~Qt::ItemIsEnabled;
              flags &= ~Qt::ItemIsSelectable;
            } else {
              flags |= Qt::ItemIsEnabled;
              flags |= Qt::ItemIsSelectable;
            }
            item->setFlags(flags);
          }
        }
      }
    }
    // Cell tooltips arrive as (plugin row, col, text); translate the row to the
    // current view order and set the tooltip on the item. A delivery that carries
    // cell_tooltips states the complete set, so clear every item tooltip first —
    // otherwise a tooltip the plugin dropped would linger on a stale cell after
    // an in-place row rewrite.
    if (cell_tooltips) {
      for (int r = 0; r < tw->rowCount(); ++r) {
        for (int c = 0; c < tw->columnCount(); ++c) {
          if (auto* item = tw->item(r, c)) {
            item->setToolTip(QString());
          }
        }
      }
      for (const auto& [plugin_row, col, tip] : *cell_tooltips) {
        if (plugin_row < 0 || static_cast<std::size_t>(plugin_row) >= plugin_to_view.size()) {
          continue;
        }
        const int view_row = plugin_to_view[static_cast<std::size_t>(plugin_row)];
        if (auto* item = tw->item(view_row, col)) {
          item->setToolTip(QString::fromStdString(tip));
        }
      }
    }
    if (selected_rows) {
      // Re-applying the selection via selectRow() scrolls the view to the last
      // selected row, so the table "jumps" on every re-render that follows a user
      // selection change (the common case, where the selection is ALREADY what we
      // want). Skip when it already matches; otherwise preserve the scroll position
      // across the change so a programmatic update (deselect-all, filter) does not
      // yank the viewport either.
      // Both sides of the comparison live in plugin row space, so a selection
      // that already matches is recognized even under a user-sorted view.
      std::set<int> want(selected_rows->begin(), selected_rows->end());
      std::set<int> have;
      for (const QModelIndex& idx : tw->selectionModel()->selectedRows()) {
        have.insert(view_to_plugin[static_cast<std::size_t>(idx.row())]);
      }
      if (want != have) {
        QScrollBar* vbar = tw->verticalScrollBar();
        const int scroll = vbar != nullptr ? vbar->value() : 0;
        tw->clearSelection();
        for (int r : *selected_rows) {
          if (r >= 0 && static_cast<std::size_t>(r) < plugin_to_view.size()) {
            tw->selectRow(plugin_to_view[static_cast<std::size_t>(r)]);
          }
        }
        if (vbar != nullptr) {
          vbar->setValue(scroll);
        }
      }
    }
    if (auto v = view.selectedItems(name)) {
      // Text-keyed selection restore (setSelectedItems): match each row by
      // tableRowKeyText — the same key the selection-changed emit uses — so the
      // restore is sort-agnostic (row indices desync under sortingEnabled) and
      // works for tables with a leading radio/widget column. Applied second so
      // it wins over selected_rows if a plugin ever sent both. Same
      // skip-if-unchanged + scroll-preservation rationale as the index path.
      std::set<std::string> want_texts(v->begin(), v->end());
      std::vector<int> want_rows;
      for (int r = 0; r < tw->rowCount(); ++r) {
        if (auto key = tableRowKeyText(tw, r); key && want_texts.count(*key) > 0) {
          want_rows.push_back(r);
        }
      }
      std::set<int> want(want_rows.begin(), want_rows.end());
      std::set<int> have;
      for (const QModelIndex& idx : tw->selectionModel()->selectedRows()) {
        have.insert(idx.row());
      }
      if (want != have) {
        QScrollBar* vbar = tw->verticalScrollBar();
        const int scroll = vbar != nullptr ? vbar->value() : 0;
        tw->clearSelection();
        for (int r : want_rows) {
          tw->selectRow(r);
        }
        if (vbar != nullptr) {
          vbar->setValue(scroll);
        }
      }
    }
    return;
  }

  // --- QLabel ---
  if (auto* lbl = qobject_cast<QLabel*>(w)) {
    if (auto v = view.label(name)) {
      lbl->setText(QString::fromStdString(*v));
    }
    // Also allow "text" for labels
    if (auto v = view.text(name)) {
      lbl->setText(QString::fromStdString(*v));
    }
    return;
  }

  // --- QPushButton ---
  if (auto* btn = qobject_cast<QPushButton*>(w)) {
    if (auto v = view.buttonText(name)) {
      btn->setText(QString::fromStdString(*v));
    }
    if (auto svg = view.buttonIconSvg(name)) {
      QByteArray svg_data = QByteArray::fromStdString(*svg);
      QSvgRenderer renderer(svg_data);
      if (renderer.isValid()) {
        int sz = btn->iconSize().height() > 0 ? btn->iconSize().height() : 16;
        QPixmap pix(sz, sz);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        renderer.render(&painter);
        btn->setIcon(QIcon(pix));
      }
    }
    // Named icons: the plugin sends a semantic id (setButtonIconNamed); the
    // host resolves it from its themed icon set. Unknown ids leave the button
    // icon untouched.
    if (auto icon_name = view.buttonIconName(name)) {
      const QString path = resolveNamedIconPath(*icon_name);
      if (!path.isEmpty()) {
        btn->setIcon(QIcon(loadSvg(path, currentTheme())));
      }
    }
    return;
  }

  // --- QTabWidget ---
  if (auto* tw = qobject_cast<QTabWidget*>(w)) {
    // Stretch the tabs across the full bar width. Load-bearing for panels that
    // set documentMode in their .ui — the only mode in which the tab bar gets
    // the full pane width — because QTabWidget::setDocumentMode() resets
    // QTabBar::expanding to false during the .ui load. Harmless for
    // non-document tab widgets (their bar stays at sizeHint, where expanding
    // has nothing to distribute).
    tw->tabBar()->setExpanding(true);
    if (tw->documentMode()) {
      // Document-mode bars paint a base line across the non-selected tabs
      // (PE_FrameTabBarBase); the app's flat tab styling has no pane frame
      // for it to connect to, so it reads as a stray line. Drop it.
      tw->tabBar()->setDrawBase(false);
    }
    if (auto v = view.tabIndex(name)) {
      tw->setCurrentIndex(*v);
    }
    return;
  }

  // --- QDialogButtonBox ---
  if (auto* dbb = qobject_cast<QDialogButtonBox*>(w)) {
    if (auto v = view.okEnabled(name)) {
      if (auto* ok = dbb->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(*v);
      }
    }
    return;
  }

  // --- RangeSlider (two-handle range slider) ---
  if (auto* rs = qobject_cast<RangeSlider*>(w)) {
    // Bounds first — setMinimum/setMaximum reset the handle values, so values
    // (sent in the same tick) must be applied afterwards.
    if (auto v = view.rangeSliderMin(name)) {
      rs->setMinimum(*v);
    }
    if (auto v = view.rangeSliderMax(name)) {
      rs->setMaximum(*v);
    }
    if (auto v = view.rangeSliderLower(name)) {
      rs->setLowerValue(*v);
    }
    if (auto v = view.rangeSliderUpper(name)) {
      rs->setUpperValue(*v);
    }
    // Time labels: when a time span is provided, float each handle's offset-from-start
    // ABOVE it (always shown) and show the selected duration as a chip ON the track.
    // The track keeps the playback scrubber's 24px height; the labels add ONE row above
    // (minimumSizeHint), with no wasted reserve below — grow the widget to fit it.
    if (auto span = view.rangeSliderTimeSpan(name)) {
      const std::int64_t min_ns = span->first;
      const std::int64_t max_ns = span->second;
      if (max_ns > min_ns) {
        const int slider_max = rs->getMaximun();
        rs->setShowHandleValueTooltip(false);
        rs->setFloatingLabelsVisible(true);
        rs->setMinimumHeight(rs->minimumSizeHint().height());
        rs->setLabelFormatter([min_ns, max_ns, slider_max](double pos) -> QString {
          std::int64_t ns = sliderToNs(static_cast<int>(pos), slider_max, min_ns, max_ns);
          return QString::fromStdString(formatDuration(ns - min_ns));
        });
        rs->setCenterLabelFormatter([min_ns, max_ns, slider_max](double lo, double hi) -> QString {
          std::int64_t lo_ns = sliderToNs(static_cast<int>(lo), slider_max, min_ns, max_ns);
          std::int64_t hi_ns = sliderToNs(static_cast<int>(hi), slider_max, min_ns, max_ns);
          return QString::fromStdString(formatDuration(hi_ns - lo_ns));
        });
        rs->update();
      }
    }
    // Boundary markers (chunk lines + labels + in-range shading). nullopt = no
    // change; an explicit empty list clears them.
    if (auto markers = view.rangeSliderMarkers(name)) {
      std::vector<RangeSlider::Marker> out;
      out.reserve(markers->size());
      for (const auto& m : *markers) {
        out.push_back({m.start, m.end, QString::fromStdString(m.label)});
      }
      rs->setMarkers(std::move(out));
    }
    return;
  }

  // --- DateRangePicker (date/time range placeholder hints) ---
  if (auto* drp = qobject_cast<DateRangePicker*>(w)) {
    if (auto iso = view.dateRangeEarliest(name)) {
      drp->setEarliestDate(QDate::fromString(QString::fromStdString(*iso), Qt::ISODate));
    }
    if (auto iso = view.dateRangeLatest(name)) {
      drp->setLatestDate(QDate::fromString(QString::fromStdString(*iso), Qt::ISODate));
    }
    return;
  }

  // --- QFrame with chart_series or chart_zoom_enabled → PlotWidget or ChartPreviewWidget ---
  if (auto* frame = qobject_cast<QFrame*>(w)) {
    auto series_data = view.chartSeries(name);
    auto zoom_enabled = view.chartZoomEnabled(name);
    auto auto_zoom = view.chartAutoZoom(name);
    if (series_data || zoom_enabled) {
      if (session != nullptr && catalog != nullptr) {
        // Full PlotWidget — zoom/tracker/legend, matching FilterEditorPanel preview quality.
        // Right-click context menu disabled per Davide's comment ("embedded PlotWidget
        // should have the right click menu disabled").
        auto* plot = frame->findChild<PJ::PlotWidget*>();
        if (!plot) {
          auto* layout = frame->layout();
          if (!layout) {
            layout = new QVBoxLayout(frame);
            layout->setContentsMargins(0, 0, 0, 4);
          }
          plot = new PJ::PlotWidget(&session->sessionManager(), catalog, frame);
          plot->setContextMenuEnabled(false);
          // PlotWidgetBase starts with the grid disabled; show it so embedded chart
          // previews match the native editor's gridded look.
          plot->setGridVisible(true);
          layout->addWidget(plot);
        }
        if (series_data) {
          // Mirror FilterEditorPanel's update strategy:
          // - Rebuild curves only when the SET of labels changes (like preview_set_key_).
          // - Otherwise just update samples and replot — preserves the user's zoom.
          // - zoomOut only on rebuild (first paint or new series set).
          // The current label set is stored as a frame property so we can detect changes.
          QStringList new_labels;
          for (const auto& s : *series_data) {
            new_labels << QString::fromStdString(s.label);
          }
          const QString new_set_key = new_labels.join(QLatin1Char('|'));
          const QString old_set_key = frame->property("_chart_set_key").toString();
          const bool rebuilt = (new_set_key != old_set_key);

          if (rebuilt) {
            // Set changed: remove all curves and re-create them with empty samples.
            plot->removeAllCurves();
            for (const auto& s : *series_data) {
              QColor color;
              if (!s.color.empty()) {
                color = QColor(QString::fromStdString(s.color));
              }
              const QString label = QString::fromStdString(s.label);
              plot->PlotWidgetBase::addCurve(
                  label, new QwtPointSeriesData(), color.isValid() ? color : Qt::transparent, label);
            }
            frame->setProperty("_chart_set_key", new_set_key);
            // Reset user-zoom flag so autozoom kicks in for the new series set.
            frame->setProperty("_user_zoomed", false);
          }

          // Update samples on existing curves (no rebuild overhead).
          {
            const auto& curve_list = plot->curveList();
            auto curve_it = curve_list.begin();
            std::size_t i = 0;
            while (curve_it != curve_list.end() && i < series_data->size()) {
              const auto& s = (*series_data)[i];
              QVector<QPointF> pts;
              pts.reserve(static_cast<int>(s.points.size()));
              for (const auto& p : s.points) {
                pts.append(QPointF(p.first, p.second));
              }
              auto* pts_data = new QwtPointSeriesData();
              pts_data->setSamples(pts);
              curve_it->curve->setData(pts_data);
              ++curve_it;
              ++i;
            }
          }

          // Re-apply the app's grid state that the host pushed as a frame property.
          // Done on EVERY update so it survives a curve rebuild. Curve style/width are
          // deliberately NOT pushed: a plugin preview has no originating plot to mirror,
          // so it keeps the PlotWidget default style/width.
          if (frame->property("_pj_view_set").toBool()) {
            plot->setGridVisible(frame->property("_pj_view_grid").toBool());
          }

          // Per-series dashed pattern LAST: a dashed series is the faded "before"
          // ghost (matches FilterEditorPanel/native TransformEditorPanel). Applied
          // after the grid config so it wins over the base pen the curve was built with.
          {
            const auto& curve_list = plot->curveList();
            auto curve_it = curve_list.begin();
            std::size_t i = 0;
            while (curve_it != curve_list.end() && i < series_data->size()) {
              if (curve_it->curve != nullptr && (*series_data)[i].dashed) {
                QPen pen = curve_it->curve->pen();
                pen.setStyle(Qt::DashLine);
                curve_it->curve->setPen(pen);
              }
              ++curve_it;
              ++i;
            }
          }

          // Connect viewResized once to detect manual user zoom.
          if (!frame->property("_zoom_connected").toBool()) {
            QObject::connect(plot, &PJ::PlotWidgetBase::viewResized, frame, [frame](const QRectF& /*r*/) {
              frame->setProperty("_user_zoomed", true);
            });
            frame->setProperty("_zoom_connected", true);
          }

          // Replot, then auto-fit. If the plugin sent an explicit AutoZoom flag,
          // honor it (true => fit every update, like the editor's AutoZoom box;
          // false => keep the user's zoom, but still fit once on a new series set).
          // Otherwise fall back to "fit until the user manually zooms".
          plot->replot();
          bool do_zoom;
          if (auto_zoom.has_value()) {
            do_zoom = rebuilt || *auto_zoom;
          } else {
            do_zoom = !frame->property("_user_zoomed").toBool();
          }
          if (do_zoom) {
            plot->zoomOut(false);
          }
        }
      } else {
        // Fallback: ChartPreviewWidget (no session/catalog available).
        auto* chart = frame->findChild<PJ::ChartPreviewWidget*>();
        if (!chart) {
          auto* layout = frame->layout();
          if (!layout) {
            layout = new QVBoxLayout(frame);
            layout->setContentsMargins(0, 0, 0, 0);
          }
          chart = new PJ::ChartPreviewWidget(frame);
          layout->addWidget(chart);
        }
        if (series_data) {
          std::vector<PJ::ChartPreviewWidget::Series> chart_series;
          chart_series.reserve(series_data->size());
          for (const auto& s : *series_data) {
            chart_series.push_back({s.label, s.points, s.color});
          }
          chart->setSeries(chart_series);
        }
        if (zoom_enabled) {
          chart->setZoomEnabled(*zoom_enabled);
        }
      }
    }
    return;
  }

  // Containers (QGroupBox, QWidget) — only generic properties applied above.
  // Warn about widget types that have data in the view but aren't handled.
  // Skip known container types that only use generic enabled/visible properties.
  // Plain QWidget is matched by EXACT type (not qobject_cast, which any widget
  // satisfies): a bare container pane taking generic show/hide is legitimate,
  // while an unhandled CUSTOM subclass still deserves the warning.
  if (!qobject_cast<QGroupBox*>(w) && !qobject_cast<QSplitter*>(w) && w->metaObject() != &QWidget::staticMetaObject) {
    qWarning(
        "WidgetBinding: unsupported widget type '%s' for '%s'; "
        "see dialog-plugin-guide.md for supported types",
        w->metaObject()->className(), std::string(name).c_str());
  }
}

void applyWidgetData(
    QWidget* root, const PJ::WidgetDataView& view, PJ::AppSession* session, PJ::CatalogModel* catalog) {
  for (const auto& name : view.widgetNames()) {
    auto* w = root->findChild<QWidget*>(QString::fromStdString(name));
    if (!w) {
      continue;
    }
    applyToWidget(w, name, view, session, catalog);
  }
  // NOTE: styled-widget adaptation is NOT re-run here on every data tick. It is
  // structural (depends on the widget tree, built once at load), so the engines
  // call adaptStyledWidgets once after loading the .ui (see widget_adapters),
  // and applyToWidget adapts reactively per widget via tryAdaptStyledWidget for
  // controls that only become adaptable after their first data arrives.
}

// ---------------------------------------------------------------------------
// connect_widget_signals — wire Qt signals to WidgetEventBuilder output
// ---------------------------------------------------------------------------

static bool isInternalWidgetName(const QString& name) {
  return name.startsWith("qt_");
}

void connectWidgetSignals(QWidget* root, WidgetEventCallback callback) {
  using PJ::WidgetEventBuilder;

  // ChartPreviewWidget instances are unnamed children of their parent QFrame.
  // Wire their viewChanged signals using the parent frame's objectName as the event widget name.
  // Must run after applyWidgetData() so charts that were created on first apply are found here.
  for (auto* chart : root->findChildren<PJ::ChartPreviewWidget*>()) {
    auto* parent_frame = qobject_cast<QFrame*>(chart->parent());
    if (!parent_frame || parent_frame->objectName().isEmpty()) {
      continue;
    }
    std::string chart_name = parent_frame->objectName().toStdString();
    QObject::connect(
        chart, &PJ::ChartPreviewWidget::viewChanged, chart,
        [callback, chart_name](double x_min, double x_max, double y_min, double y_max) {
          callback(chart_name, WidgetEventBuilder::chartViewChanged(x_min, x_max, y_min, y_max));
        });
  }

  for (auto* w : root->findChildren<QWidget*>()) {
    QString qname = w->objectName();
    if (qname.isEmpty() || isInternalWidgetName(qname)) {
      continue;
    }
    std::string name = qname.toStdString();

    if (auto* le = qobject_cast<QLineEdit*>(w)) {
      QObject::connect(le, &QLineEdit::textChanged, le, [callback, name](const QString& text) {
        callback(name, WidgetEventBuilder::textChanged(text.toStdString()));
      });
      continue;
    }
    if (auto* pte = qobject_cast<QPlainTextEdit*>(w)) {
      // Only wire code editors (marked by _pj_code_lang property), not read-only plain text.
      if (pte->property("_pj_code_lang").isValid()) {
        // Caret-tracking editors (opt-in via setCodeCaretTracking) emit code +
        // caret offset on both edits and cursor moves, so caret-aware completion
        // can react to the cursor even when the text didn't change. Editors that
        // didn't opt in fire on text changes only and carry no caret — the
        // pre-caret behavior — so an editor that merely validates code isn't
        // re-run on every cursor move.
        if (pte->property("_pj_caret_tracking").toBool()) {
          auto emit_code = [callback, name, pte]() {
            callback(
                name, WidgetEventBuilder::codeChanged(pte->toPlainText().toStdString(), pte->textCursor().position()));
          };
          QObject::connect(pte, &QPlainTextEdit::textChanged, pte, emit_code);
          QObject::connect(pte, &QPlainTextEdit::cursorPositionChanged, pte, emit_code);
        } else {
          QObject::connect(pte, &QPlainTextEdit::textChanged, pte, [callback, name, pte]() {
            callback(name, WidgetEventBuilder::codeChanged(pte->toPlainText().toStdString()));
          });
        }
      }
      continue;
    }
    if (auto* cb = qobject_cast<QComboBox*>(w)) {
      QObject::connect(cb, &QComboBox::currentIndexChanged, cb, [callback, name, cb](int index) {
        callback(name, WidgetEventBuilder::indexChanged(index, cb->currentText().toStdString()));
      });
      // Editable combos let the user type a free value (server URI, etc.).
      // currentIndexChanged alone never fires while typing, and the typed
      // dispatcher routes index events to onIndexChanged — so a plugin reading
      // the value via onTextChanged would never see it. editTextChanged fires
      // for both typing and dropdown selection (selection updates the line
      // edit), so forward it as a text_changed event. currentIndexChanged is
      // kept for index-based consumers (onIndexChanged).
      if (cb->isEditable()) {
        QObject::connect(cb, &QComboBox::editTextChanged, cb, [callback, name](const QString& text) {
          callback(name, WidgetEventBuilder::textChanged(text.toStdString()));
        });
      }
      continue;
    }
    if (auto* ck = qobject_cast<QCheckBox*>(w)) {
      QObject::connect(ck, &QCheckBox::toggled, ck, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* ts = qobject_cast<ToggleSwitch*>(w)) {
      QObject::connect(ts, &ToggleSwitch::toggled, ts, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* rb = qobject_cast<QRadioButton*>(w)) {
      QObject::connect(rb, &QRadioButton::toggled, rb, [callback, name](bool checked) {
        callback(name, WidgetEventBuilder::toggled(checked));
      });
      continue;
    }
    if (auto* sb = qobject_cast<QSpinBox*>(w)) {
      QObject::connect(sb, &QSpinBox::valueChanged, sb, [callback, name](int value) {
        callback(name, WidgetEventBuilder::valueChanged(value));
      });
      continue;
    }
    if (auto* dsb = qobject_cast<QDoubleSpinBox*>(w)) {
      QObject::connect(dsb, &QDoubleSpinBox::valueChanged, dsb, [callback, name](double value) {
        callback(name, WidgetEventBuilder::valueChanged(value));
      });
      continue;
    }
    if (auto* lw = qobject_cast<QListWidget*>(w)) {
      QObject::connect(lw, &QListWidget::itemSelectionChanged, lw, [callback, name, lw]() {
        std::vector<std::string> sel;
        for (auto* item : lw->selectedItems()) {
          sel.push_back(item->text().toStdString());
        }
        callback(name, WidgetEventBuilder::selectionChanged(sel));
      });
      QObject::connect(lw, &QListWidget::itemDoubleClicked, lw, [callback, name, lw](QListWidgetItem* item) {
        // Report the delivered-order index, not the (possibly sorted) view row.
        callback(name, WidgetEventBuilder::itemDoubleClicked(listItemPluginIndex(lw, item)));
      });
      continue;
    }
    if (auto* tw = qobject_cast<QTableWidget*>(w)) {
      QObject::connect(tw, &QTableWidget::itemSelectionChanged, tw, [callback, name, tw]() {
        // Emit one entry per selected row, keyed by tableRowKeyText (see its
        // doc comment for why a fixed column 0 doesn't work). This is the same
        // key the selected_items apply path matches rows by, so the two
        // directions stay in sync.
        std::vector<std::string> sel;
        std::vector<int> seen_rows;
        for (auto* item : tw->selectedItems()) {
          const int row = item->row();
          bool dup = false;
          for (int r : seen_rows) {
            if (r == row) {
              dup = true;
              break;
            }
          }
          if (dup) {
            continue;
          }
          seen_rows.push_back(row);
          if (auto key = tableRowKeyText(tw, row)) {
            sel.push_back(*key);
          }
        }
        callback(name, WidgetEventBuilder::selectionChanged(sel));
      });
      // Double-click a row -> itemDoubleClicked(row), mirroring QListWidget so a
      // plugin can implement double-click-to-use on a table (e.g. the function
      // library box). Emits the plugin-order index of the double-clicked row,
      // translated from the (possibly user-sorted) view position.
      QObject::connect(tw, &QTableWidget::cellDoubleClicked, tw, [callback, name, tw](int row, int /*col*/) {
        callback(name, WidgetEventBuilder::itemDoubleClicked(viewRowToPluginRow(tw, row)));
      });
      // Stash the event callback so applyTableRadioColumn can wire radio cells
      // (created lazily as rows arrive) back to the dialog event stream. The
      // clicked radio resolves to a view row; the plugin expects its own order.
      new RadioEmitHolder(tw, [callback, name, tw](int row) {
        callback(name, WidgetEventBuilder::tableRadioSelected(viewRowToPluginRow(tw, row)));
      });
      continue;
    }
    if (auto* btn = qobject_cast<QPushButton*>(w)) {
      // Skip buttons that are part of QDialogButtonBox
      if (qobject_cast<QDialogButtonBox*>(btn->parent())) {
        continue;
      }
      QObject::connect(
          btn, &QPushButton::clicked, btn, [callback, name]() { callback(name, WidgetEventBuilder::clicked()); });
      continue;
    }
    if (auto* tw = qobject_cast<QTabWidget*>(w)) {
      QObject::connect(tw, &QTabWidget::currentChanged, tw, [callback, name](int index) {
        callback(name, WidgetEventBuilder::tabChanged(index));
      });
      continue;
    }
    if (auto* rs = qobject_cast<RangeSlider*>(w)) {
      // Both handle signals coalesce into one rangeChanged event carrying the
      // current lower+upper, so dragging either handle keeps the plugin in sync.
      auto emit_range = [callback, name, rs]() {
        callback(name, WidgetEventBuilder::rangeChanged(rs->getLowerValue(), rs->getUpperValue()));
      };
      QObject::connect(rs, &RangeSlider::lowerValueChanged, rs, [emit_range](int) { emit_range(); });
      QObject::connect(rs, &RangeSlider::upperValueChanged, rs, [emit_range](int) { emit_range(); });
      continue;
    }
    if (auto* drp = qobject_cast<DateRangePicker*>(w)) {
      QObject::connect(drp, &DateRangePicker::filterChanged, drp, [callback, name](const RangeFilter& f) {
        // Combine date + time into UTC ISO datetimes; empty string = unbounded side.
        std::string from_iso;
        std::string to_iso;
        if (f.date_from.has_value()) {
          from_iso = QDateTime(*f.date_from, f.from_time, QTimeZone::utc()).toString(Qt::ISODate).toStdString();
        }
        if (f.date_to.has_value()) {
          to_iso = QDateTime(*f.date_to, f.to_time, QTimeZone::utc()).toString(Qt::ISODate).toStdString();
        }
        callback(name, WidgetEventBuilder::dateRangeChanged(from_iso, to_iso));
      });
      continue;
    }
  }
}

// ---------------------------------------------------------------------------
// installButtonShortcuts — create QShortcuts for buttons declaring a shortcut
// ---------------------------------------------------------------------------

void installButtonShortcuts(QWidget* root, const PJ::WidgetDataView& view) {
  for (const auto& name : view.widgetNames()) {
    auto sc = view.shortcut(name);
    if (!sc) {
      continue;
    }
    auto* btn = root->findChild<QPushButton*>(QString::fromStdString(name));
    if (!btn) {
      continue;
    }
    auto* shortcut = new QShortcut(QKeySequence(QString::fromStdString(*sc)), root);
    QObject::connect(shortcut, &QShortcut::activated, btn, &QPushButton::click);
  }
}

}  // namespace PJ
