// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <pj_widgets/Dialog.h>
#include <pj_widgets/FileDialog.h>
#include <pj_widgets/SectionHeaderBand.h>

#include <QAbstractItemView>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QLayout>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSpacerItem>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host/widget_event_builder.hpp>
#include <pj_plugins/host_qt/dialog_engine.hpp>
#include <pj_plugins/host_qt/drop_event_filter.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_adapters.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

// Size every SectionHeaderBand in a freshly-loaded plugin UI to the app's
// canonical band height. QUiLoader inflates them at the widget default height
// with no wiring to the host, so without this they render shorter than the
// panel-hosted toolboxes' bands. No-op when metrics are unset (headless/tests).
void applySectionBandMetrics(QWidget* root, const std::optional<ChromeMetrics>& metrics) {
  if (root == nullptr || !metrics.has_value()) {
    return;
  }
  for (auto* band : root->findChildren<SectionHeaderBand*>()) {
    band->onChromeMetricsChanged(*metrics);
  }
}

}  // namespace

DialogEngine::DialogEngine(PJ::DialogHandle handle, DialogEngineConfig config)
    : handle_(std::move(handle)), config_(config) {}

// ---------------------------------------------------------------------------
// JSON diff: compute which widget keys changed between old and new data
// ---------------------------------------------------------------------------

static nlohmann::json computeDiff(const nlohmann::json& old_data, const nlohmann::json& new_data) {
  nlohmann::json diff = nlohmann::json::object();
  for (const auto& [key, val] : new_data.items()) {
    if (!old_data.contains(key) || old_data[key] != val) {
      diff[key] = val;
    }
  }
  return diff;
}

// ---------------------------------------------------------------------------
// apply_and_diff: re-read widget data, apply (diffed or full), update prev
// ---------------------------------------------------------------------------

/// Holds the result of applying widget data: whether accept was requested and
/// whether a sub-dialog was requested (with its UI XML).
struct ApplyResult {
  bool wants_accept = false;
  std::optional<std::string> sub_dialog_ui;
};

/// The engine's memory of the last payload delivered to a widget tree: the
/// parsed per-widget state (key-diffing) plus the raw bytes (byte-identical
/// skip guard). The two are always mutated together.
struct PayloadMemo {
  nlohmann::json data = nlohmann::json::object();
  std::string raw;
};

static ApplyResult applyAndDiff(
    QWidget* root, PJ::DialogHandle& handle, PayloadMemo& memo, const PJ::DialogEngineConfig& config,
    PJ::DialogEngine::Stats& stats) {
  const bool enable_diff = config.enable_diff;
  int& diff_apply_count = stats.diff_apply_count;
  std::string raw = handle.widget_data();
  // A plugin that reports "changed" but re-emits byte-identical widget data
  // pays nothing: skip the parse + diff + apply (same guard as PanelEngine).
  // One-shot requests (accept/sub-dialog) flip the bytes, so they still fire.
  if (raw == memo.raw) {
    ++stats.skipped_identical_count;
    return {};
  }
  memo.raw = raw;
  nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
  if (new_data.is_discarded()) {
    return {};
  }

  PJ::WidgetDataView full_view(raw);
  ApplyResult result;
  result.wants_accept = full_view.requestAccept();
  result.sub_dialog_ui = full_view.subDialogUi();

  // Strip commands before diffing (they're one-shot)
  new_data.erase("__request_accept");
  new_data.erase("__request_sub_dialog");

  if (enable_diff) {
    nlohmann::json diff = computeDiff(memo.data, new_data);
    if (!diff.empty()) {
      PJ::WidgetDataView view(diff.dump());
      applyWidgetData(root, view);
      ++diff_apply_count;
    }
  } else {
    applyWidgetData(root, full_view);
  }
  memo.data = std::move(new_data);
  return result;
}

// ---------------------------------------------------------------------------
// show_dialog
// ---------------------------------------------------------------------------

DialogResult DialogEngine::showDialog(QWidget* parent) {
  stats_ = {};

  // 1. Load .ui
  std::string ui = handle_.ui_content();
  QByteArray data(ui.data(), static_cast<int>(ui.size()));
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);

  PjUiLoader loader;
  QWidget* loaded = loader.load(&buffer, parent);
  if (!loaded) {
    return DialogResult::kRejected;
  }
  adaptStyledWidgets(loaded);

  // 2. Wrap in the app's canonical frameless chrome. Plugin UIs are declarative
  //    and rendered by the host, so the host owns their window chrome: they get
  //    the same custom title bar / drag / resize / close as every app dialog,
  //    never native GNOME decorations. `loaded` is embedded as the content
  //    regardless of whether its .ui root was a QDialog or a plain QWidget.
  auto* dialog = new PJ::Dialog(parent);
  // Match the host's live chrome metrics so the plugin dialog's title bar is the
  // same height as the main window (falls back to the shared defaults otherwise).
  if (config_.section_band_metrics) {
    dialog->setChromeMetrics(*config_.section_band_metrics);
  }
  dialog->setDialogTitle(loaded->windowTitle());
  dialog->contentLayout()->addWidget(loaded);
  // A QDialog-rooted .ui swallows Esc itself; forward its close to the chrome.
  forwardEmbeddedDialogClose(loaded, dialog);

  // Dialogs with a parser slot embed a parser-options widget whose height varies
  // with the chosen message protocol (a single msgpack checkbox vs a tall
  // Protobuf table). A fixed .ui size can't fit both without dead space or
  // clipping, so for these — and only these — the host sizes the dialog to its
  // actual content at runtime. Plain dialogs (topic-table loaders, toolboxes…)
  // have no parser slot and keep their authored .ui size untouched.
  const bool content_fit_dialog = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;

  // Snap the dialog to its real content: drop the authored fixed minimum heights
  // (the dialog's own and the parser slot's reservation) and collapse expanding
  // vertical spacers so there's no dead space, but give tables/lists/editors a
  // readable minimum so they don't shrink to a tiny default. Idempotent — safe
  // to call on every protocol change.
  auto fit_to_content = [loaded, dialog]() {
    QWidget* slot = loaded->findChild<QWidget*>("pj_parser_slot");
    if (slot == nullptr) {
      return;
    }
    // Clear BOTH width and height minimums (not just height): the floor set at the
    // end of a previous run — setMinimumSize(minimumSizeHint()) — pins width too, so
    // a wider parser's minimum width would otherwise stick and prevent a later,
    // narrower parser from shrinking the dialog back. Resetting only height is what
    // made the final size depend on the order parsers were selected.
    loaded->setMinimumSize(0, 0);
    dialog->setMinimumSize(0, 0);
    slot->setMinimumSize(0, 0);
    std::function<void(QLayout*)> collapse_vspacers = [&](QLayout* layout) {
      if (layout == nullptr) {
        return;
      }
      for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (QSpacerItem* spacer = item->spacerItem()) {
          if (spacer->expandingDirections() & Qt::Vertical) {
            // Zero the spacer's size hint so it contributes no dead space to
            // adjustSize (the dialog still snaps to its content), but keep it
            // VERTICALLY EXPANDING so that when a pane is taller than its content
            // — a short parser page under a tall topic list, a few-option
            // serialization page — the spacer absorbs the surplus and the
            // settings stay pinned to the top instead of spreading apart.
            spacer->changeSize(
                theme::space(theme::Space::None), theme::space(theme::Space::None), QSizePolicy::Minimum,
                QSizePolicy::Expanding);
          }
        } else if (QLayout* child = item->layout()) {
          collapse_vspacers(child);
        }
      }
      layout->invalidate();
    };
    collapse_vspacers(loaded->layout());
    // QSplitter (and other composite widgets) hold their children as widgets, not
    // as layout items, so the recursion above never reaches spacers nested inside
    // a splitter pane. Collapse the vertical spacers in every descendant widget's
    // own layout too — otherwise a splitter-based dialog keeps its dead space and
    // mis-sizes (e.g. the MQTT dialog's Security column gap, which in turn clipped
    // the list placeholder overlay).
    for (QWidget* descendant : loaded->findChildren<QWidget*>()) {
      collapse_vspacers(descendant->layout());
    }
    // Parser options that carry a table (Protobuf, ROS1/ROS2) need room to be
    // usable; give such tables/editors a generous minimum height. Protocols
    // without a table (msgpack/cbor/json) have no item view here and stay
    // compact.
    const auto ensure_readable = [](QWidget* w) {
      if (w->minimumHeight() < 300) {
        w->setMinimumHeight(300);
      }
    };
    for (QAbstractItemView* view : loaded->findChildren<QAbstractItemView*>()) {
      ensure_readable(view);
    }
    for (QPlainTextEdit* editor : loaded->findChildren<QPlainTextEdit*>()) {
      ensure_readable(editor);
    }
    // Force a fresh layout pass so the size hints reflect the just-injected parser,
    // then explicitly size the dialog to that content. With the minimums cleared in
    // both dimensions above, this snaps to the correct size for the selected parser
    // every time, independent of the order parsers were selected.
    if (dialog->layout() != nullptr) {
      dialog->layout()->activate();
    }
    dialog->adjustSize();
    // Re-establish a resize floor at the content minimum. The minimum resets
    // above let the dialog re-measure smaller; leaving the floor at 0 is what let
    // the user drag the window below what its widgets need — a QSplitter squeezed
    // under its panes' minimum crushes their rows into overlapping text (the
    // Connection/Security grids) and squashes the parser list so its placeholder
    // clips. minimumSizeHint is layout-derived (grids + readable item views, with
    // dead-space spacers already collapsed), so the floor is the true crush point
    // and tracks the current parser. Recomputed on every protocol change.
    dialog->setMinimumSize(dialog->minimumSizeHint());
  };

  // Wire buttonBox signals — works whether the loaded widget was a QDialog
  // or a plain QWidget. Needed so Close/OK/Cancel buttons function correctly.
  {
    auto* button_box = loaded->findChild<QDialogButtonBox*>("buttonBox");
    if (button_box) {
      QObject::connect(button_box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
      QObject::connect(button_box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    } else {
      qWarning(
          "DialogEngine: no QDialogButtonBox named 'buttonBox' found in UI. "
          "OK/Cancel buttons will not work. Ensure your .ui XML has: "
          "<widget class=\"QDialogButtonBox\" name=\"buttonBox\">");
    }
  }

  // Restore saved dialog geometry (keyed by plugin manifest name)
  auto manifest_json = nlohmann::json::parse(handle_.manifest(), nullptr, false);
  std::string plugin_name = manifest_json.is_object() ? manifest_json.value("name", "") : "";
  QString geometry_key = QString("DialogGeometry/%1").arg(QString::fromStdString(plugin_name));
  // Content-fit dialogs never restore a saved size — it would override the fit
  // (a stale large geometry re-applied on show is exactly what left dead space).
  if (!plugin_name.empty() && !content_fit_dialog) {
    QSettings settings;
    auto saved = settings.value(geometry_key).toByteArray();
    if (!saved.isEmpty()) {
      dialog->restoreGeometry(saved);
    }
  }

  // Task 7: detect parser slot widget
  stats_.has_parser_slot = loaded->findChild<QWidget*>("pj_parser_slot") != nullptr;

  // --- Parser slot injection state ---
  QWidget* parser_slot = nullptr;
  QWidget* parser_slot_container = nullptr;  // Parent GroupBox to show/hide
  QVBoxLayout* parser_slot_layout = nullptr;
  QWidget* parser_dialog_widget = nullptr;
  std::unique_ptr<PJ::DialogHandle> parser_dialog_handle;
  PayloadMemo parser_memo;

  // Parameterized file/folder picker handlers (work for any dialog handle)
  auto show_file_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                  PayloadMemo& target_memo) {
    if (!config_.enable_file_picker || !handle) {
      return;
    }
    PJ::WidgetDataView view(handle->widget_data());
    if (!view.isFilePicker(widget_name)) {
      return;
    }
    auto filter = view.filePickerFilter(widget_name).value_or("");
    auto title = view.filePickerTitle(widget_name).value_or("Select File");
    QString path = PJ::FileDialog::getOpenFileName(
        dialog, QString::fromStdString(title), QString(), QString::fromStdString(filter));
    if (!path.isEmpty()) {
      if (handle->sendEvent(widget_name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()))) {
        std::string raw = handle->widget_data();
        nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
        if (!new_data.is_discarded()) {
          new_data.erase("__request_accept");
          new_data.erase("__request_sub_dialog");
          PJ::WidgetDataView v(raw);
          applyWidgetData(target_widget, v);
          target_memo.data = std::move(new_data);
          // Invalidate rather than memoize: this direct path does not extract
          // one-shot commands, so the next poll must re-parse to fire them.
          target_memo.raw.clear();
        }
      }
    }
  };

  auto show_folder_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                    PayloadMemo& target_memo) {
    if (!config_.enable_file_picker || !handle) {
      return;
    }
    PJ::WidgetDataView view(handle->widget_data());
    if (!view.isFolderPicker(widget_name)) {
      return;
    }
    auto title = view.folderPickerTitle(widget_name).value_or("Select Folder");
    QString path = PJ::FileDialog::getExistingDirectory(dialog, QString::fromStdString(title));
    if (!path.isEmpty()) {
      if (handle->sendEvent(widget_name, PJ::WidgetEventBuilder::folderSelected(path.toStdString()))) {
        std::string raw = handle->widget_data();
        nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
        if (!new_data.is_discarded()) {
          new_data.erase("__request_accept");
          new_data.erase("__request_sub_dialog");
          PJ::WidgetDataView v(raw);
          applyWidgetData(target_widget, v);
          target_memo.data = std::move(new_data);
          // Invalidate rather than memoize: this direct path does not extract
          // one-shot commands, so the next poll must re-parse to fire them.
          target_memo.raw.clear();
        }
      }
    }
  };

  auto show_save_file_picker_for = [&](const std::string& widget_name, PJ::DialogHandle* handle, QWidget* target_widget,
                                       PayloadMemo& target_memo) {
    if (!config_.enable_file_picker || !handle) {
      return;
    }
    PJ::WidgetDataView view(handle->widget_data());
    if (!view.isSaveFilePicker(widget_name)) {
      return;
    }
    auto filter = view.filePickerFilter(widget_name).value_or("");
    auto title = view.filePickerTitle(widget_name).value_or("Save File");
    auto suffix = view.saveFilePickerDefaultSuffix(widget_name).value_or("");
    QString path = PJ::FileDialog::getSaveFileName(
        dialog, QString::fromStdString(title), QString(), QString::fromStdString(filter),
        QString::fromStdString(suffix));
    if (!path.isEmpty()) {
      if (handle->sendEvent(widget_name, PJ::WidgetEventBuilder::fileSelected(path.toStdString()))) {
        std::string raw = handle->widget_data();
        nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
        if (!new_data.is_discarded()) {
          new_data.erase("__request_accept");
          new_data.erase("__request_sub_dialog");
          PJ::WidgetDataView v(raw);
          applyWidgetData(target_widget, v);
          target_memo.data = std::move(new_data);
          // Invalidate rather than memoize: this direct path does not extract
          // one-shot commands, so the next poll must re-parse to fire them.
          target_memo.raw.clear();
        }
      }
    }
  };

  // Lambda: inject parser dialog for the given encoding
  auto inject_parser_dialog = [&](const QString& encoding) {
    // 1. Clear previous parser dialog
    if (parser_dialog_widget) {
      parser_slot_layout->removeWidget(parser_dialog_widget);
      delete parser_dialog_widget;
      parser_dialog_widget = nullptr;
    }
    parser_dialog_handle.reset();
    parser_memo = {};

    // 2. Query parser dialog vtable via provider
    if (!config_.parser_dialog_provider) {
      if (parser_slot_container) {
        parser_slot_container->setVisible(false);
      }
      return;
    }

    const PJ_dialog_vtable_t* vtable = config_.parser_dialog_provider(encoding.toStdString());
    if (vtable == nullptr) {
      // Parser has no dialog - hide the container
      if (parser_slot_container) {
        parser_slot_container->setVisible(false);
      }
      return;
    }

    // 3. Create parser dialog handle
    parser_dialog_handle = std::make_unique<PJ::DialogHandle>(vtable);

    // 3b. Load initial parser config if provided (restores previous state)
    if (!config_.initial_parser_config.empty()) {
      (void)parser_dialog_handle->load_config(config_.initial_parser_config);
    }

    // 4. Load parser dialog UI
    std::string parser_ui = parser_dialog_handle->ui_content();
    QByteArray parser_data(parser_ui.data(), static_cast<int>(parser_ui.size()));
    QBuffer parser_buffer(&parser_data);
    parser_buffer.open(QIODevice::ReadOnly);

    PjUiLoader parser_loader;
    parser_dialog_widget = parser_loader.load(&parser_buffer, parser_slot);
    if (!parser_dialog_widget) {
      parser_dialog_handle.reset();
      if (parser_slot_container) {
        parser_slot_container->setVisible(false);
      }
      return;
    }
    adaptStyledWidgets(parser_dialog_widget);

    // 5. Insert into slot and show container
    parser_slot_layout->addWidget(parser_dialog_widget);
    if (parser_slot_container) {
      parser_slot_container->setVisible(true);
    }
    stats_.parser_dialog_injected = true;

    // 6. Apply initial parser widget data
    std::string parser_initial_raw = parser_dialog_handle->widget_data();
    parser_memo.raw = parser_initial_raw;
    parser_memo.data = nlohmann::json::parse(parser_initial_raw, nullptr, false);
    if (parser_memo.data.is_discarded()) {
      parser_memo.data = nlohmann::json::object();
    }
    {
      PJ::WidgetDataView view(parser_initial_raw);
      applyWidgetData(parser_dialog_widget, view);
    }

    // 7. Wire parser dialog signals (events go to parser handle)
    connectWidgetSignals(parser_dialog_widget, [&](const std::string& name, const std::string& event_json) {
      stats_.event_count++;
      if (parser_dialog_handle && parser_dialog_handle->sendEvent(name, event_json)) {
        // Parser dialogs have no accept/sub-dialog tail: the one-shot commands
        // applyAndDiff extracts are deliberately ignored here.
        (void)applyAndDiff(parser_dialog_widget, *parser_dialog_handle, parser_memo, config_, stats_);
      }

      // Handle file/folder pickers in parser dialog
      show_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
      show_folder_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
      show_save_file_picker_for(name, parser_dialog_handle.get(), parser_dialog_widget, parser_memo);
    });
  };

  // Setup parser slot if detected and provider is available
  if (stats_.has_parser_slot && config_.parser_dialog_provider) {
    parser_slot = loaded->findChild<QWidget*>("pj_parser_slot");
    if (parser_slot) {
      parser_slot_container = parser_slot->parentWidget();  // Usually a QGroupBox
      parser_slot_layout = new QVBoxLayout(parser_slot);
      parser_slot_layout->setContentsMargins(
          theme::space(theme::Space::None), theme::space(theme::Space::None), theme::space(theme::Space::None),
          theme::space(theme::Space::None));

      // Connect encoding combo to trigger parser dialog injection, then re-fit.
      // The fit is deferred with singleShot(0) so it runs AFTER the layout has
      // measured the freshly-injected widget — calling it synchronously here
      // would size the dialog to the PREVIOUS protocol's options (a one-step lag,
      // where each selection shows the prior protocol's size).
      if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
        QObject::connect(combo, &QComboBox::currentTextChanged, dialog, [&, fit_to_content](const QString& encoding) {
          inject_parser_dialog(encoding);
          QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
        });
        // Note: initial injection happens AFTER widget_data is applied (see below)
      }
    }
  }

  QWidget* binding_root = loaded;

  // 3. Apply initial widget data
  std::string initial_raw = handle_.widget_data();
  PayloadMemo memo;
  memo.raw = initial_raw;
  memo.data = nlohmann::json::parse(initial_raw, nullptr, false);
  if (memo.data.is_discarded()) {
    memo.data = nlohmann::json::object();
  }
  {
    PJ::WidgetDataView view(initial_raw);
    applyWidgetData(binding_root, view);
  }

  // 3b. Trigger initial parser dialog injection now that combo is populated,
  // then size the dialog to its content.
  if (parser_slot != nullptr) {
    if (auto* combo = loaded->findChild<QComboBox*>("comboBoxProtocol")) {
      inject_parser_dialog(combo->currentText());
    }
  }
  fit_to_content();

  // Bands are in their final tree now (initial parser injection done); size them
  // to the canonical app band height so plugin dialogs match the toolboxes.
  applySectionBandMetrics(loaded, config_.section_band_metrics);

  // Helper: open a sub-dialog from UI XML (nested modal inside parent)
  auto maybe_open_sub_dialog = [&](const ApplyResult& ar) {
    if (!ar.sub_dialog_ui) {
      return;
    }

    QByteArray sub_data(ar.sub_dialog_ui->data(), static_cast<int>(ar.sub_dialog_ui->size()));
    QBuffer sub_buffer(&sub_data);
    sub_buffer.open(QIODevice::ReadOnly);

    PjUiLoader sub_loader;
    QWidget* sub_loaded = sub_loader.load(&sub_buffer, dialog);
    if (!sub_loaded) {
      return;
    }
    adaptStyledWidgets(sub_loaded);
    applySectionBandMetrics(sub_loaded, config_.section_band_metrics);

    auto* sub_dialog = new PJ::Dialog(dialog);
    sub_dialog->setDialogTitle(sub_loaded->windowTitle());
    sub_dialog->contentLayout()->addWidget(sub_loaded);
    forwardEmbeddedDialogClose(sub_loaded, sub_dialog);
    if (auto* sub_bb = sub_loaded->findChild<QDialogButtonBox*>("buttonBox")) {
      QObject::connect(sub_bb, &QDialogButtonBox::accepted, sub_dialog, &QDialog::accept);
      QObject::connect(sub_bb, &QDialogButtonBox::rejected, sub_dialog, &QDialog::reject);
    }

    sub_dialog->exec();
    delete sub_dialog;
  };

  // Shared tail for every applyAndDiff call site: accept ends the dialog
  // (returns true), otherwise a requested sub-dialog opens.
  auto handle_apply_result = [&](const ApplyResult& ar) {
    if (ar.wants_accept) {
      dialog->accept();
      return true;
    }
    maybe_open_sub_dialog(ar);
    return false;
  };

  // 5. Wire signals
  connectWidgetSignals(binding_root, [&](const std::string& name, const std::string& event_json) {
    stats_.event_count++;
    if (handle_.sendEvent(name, event_json)) {
      if (handle_apply_result(applyAndDiff(binding_root, handle_, memo, config_, stats_))) {
        return;
      }
    }
    show_file_picker_for(name, &handle_, binding_root, memo);
    show_folder_picker_for(name, &handle_, binding_root, memo);
    show_save_file_picker_for(name, &handle_, binding_root, memo);
  });

  // 5b. Install button keyboard shortcuts declared in widget data
  {
    PJ::WidgetDataView shortcut_view(handle_.widget_data());
    installButtonShortcuts(dialog, shortcut_view);
  }

  // 5c. Install drop event filter for declared drop targets
  {
    PJ::WidgetDataView drop_view(initial_raw);
    auto targets = drop_view.dropTargets();
    if (!targets.empty()) {
      auto* drop_filter = new DropEventFilter(dialog, [&](const std::string& name, const std::string& event_json) {
        stats_.event_count++;
        if (handle_.sendEvent(name, event_json)) {
          if (handle_apply_result(applyAndDiff(binding_root, handle_, memo, config_, stats_))) {
            return;
          }
        }
      });
      for (const auto& t : targets) {
        drop_filter->addTarget(t);
      }
    }
  }

  // 6. Start tick timer
  QTimer tick_timer;
  tick_timer.setInterval(config_.tick_interval_ms);
  QObject::connect(&tick_timer, &QTimer::timeout, [&]() {
    stats_.tick_count++;
    if (handle_.tick()) {
      // The tick path shares the same tail: the skip guard consumes a
      // payload's first delivery, so a command missed here would have no
      // identical-bytes retry via a later event to fall back on.
      handle_apply_result(applyAndDiff(binding_root, handle_, memo, config_, stats_));
    }
  });
  tick_timer.start();

  // Re-fit once the dialog is actually shown and laid out: the pre-show sizeHint
  // is stale (measured before the window exists), so a deferred singleShot(0)
  // fit gives the right size from the start. We only RE-FIT here — the parser
  // options were already injected and populated above, so we must NOT re-inject
  // (that would replace the populated widget with a fresh, empty one).
  if (content_fit_dialog) {
    QTimer::singleShot(0, dialog, [fit_to_content]() { fit_to_content(); });
  }

  // 7. Run dialog (modal or non-modal)
  int result;
  if (config_.non_modal) {
    dialog->setWindowModality(Qt::NonModal);
    dialog->show();
    dialog->activateWindow();
    QEventLoop loop;
    QObject::connect(dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    loop.exec();
    result = dialog->result();
  } else {
    result = dialog->exec();
  }
  tick_timer.stop();

  // 8. Notify plugin and clean up
  DialogResult dr;
  if (result == QDialog::Accepted) {
    handle_.accept(handle_.save_config());
    // Save parser config if a parser dialog was shown
    if (parser_dialog_handle) {
      parser_config_ = parser_dialog_handle->save_config();
    } else {
      parser_config_.clear();
    }
    dr = DialogResult::kAccepted;
  } else {
    handle_.reject();
    parser_config_.clear();
    dr = DialogResult::kRejected;
  }
  // Save dialog geometry for next time (not for content-fit dialogs — they
  // always size to content, so a remembered manual size must not stick).
  if (!plugin_name.empty() && !content_fit_dialog) {
    QSettings settings;
    settings.setValue(geometry_key, dialog->saveGeometry());
  }

  delete dialog;
  return dr;
}

// ---------------------------------------------------------------------------
// run_headless
// ---------------------------------------------------------------------------

std::string DialogEngine::savedConfig() const {
  return handle_.save_config();
}

std::string DialogEngine::parserConfig() const {
  return parser_config_;
}

std::string DialogEngine::runHeadless(int max_ticks) {
  for (int i = 0; i < max_ticks; ++i) {
    (void)handle_.tick();
  }
  return handle_.widget_data();
}

}  // namespace PJ
