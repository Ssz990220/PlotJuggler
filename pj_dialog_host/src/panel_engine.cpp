// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>
#include <QUiLoader>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include <pj_plugins/host/widget_data_view.hpp>
#include <pj_plugins/host_qt/drop_event_filter.hpp>
#include <pj_plugins/host_qt/panel_engine.hpp>
#include <pj_plugins/host_qt/pj_ui_loader.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>
#include <utility>

namespace PJ {

struct PanelEngine::Impl {
  // DialogHandle has no default ctor; construct Impl with the handle.
  Impl(DialogHandle h, PanelEngineConfig c) : handle(std::move(h)), config(c) {}

  DialogHandle handle;
  PanelEngineConfig config;
  QPointer<QWidget> root;
  QTimer* tick_timer = nullptr;
  nlohmann::json prev_data = nlohmann::json::object();
  std::string prev_raw;
  std::function<void(std::string)> close_cb;
  Stats stats;
  bool closed = false;

  // Same diff-and-apply cycle as DialogEngine, minus the QDialog::accept hook.
  // Returns the close-reason if the plugin requested close on this tick.
  std::optional<std::string> applyAndDiff() {
    std::string raw = handle.widget_data();
    if (raw.empty()) {
      return std::nullopt;
    }
    // Panels tick at 20Hz; an idle plugin re-emits byte-identical widget data
    // each tick. Skip the parse + per-key diff + apply when nothing changed —
    // one-shot requests (close/sub-dialog) flip the bytes, so they still fire.
    if (raw == prev_raw) {
      return std::nullopt;
    }
    prev_raw = raw;
    nlohmann::json new_data = nlohmann::json::parse(raw, nullptr, false);
    if (new_data.is_discarded()) {
      return std::nullopt;
    }
    WidgetDataView view(raw);
    auto close_reason = view.requestClose();
    auto sub_dialog_ui = view.subDialogUi();

    // Strip one-shot commands before diffing.
    new_data.erase("__request_close");
    new_data.erase("__request_sub_dialog");
    new_data.erase("__request_accept");

    if (config.enable_diff) {
      nlohmann::json diff = nlohmann::json::object();
      for (const auto& [key, val] : new_data.items()) {
        if (!prev_data.contains(key) || prev_data[key] != val) {
          diff[key] = val;
        }
      }
      if (!diff.empty() && root != nullptr) {
        WidgetDataView diff_view(diff.dump());
        applyWidgetData(root, diff_view);
        ++stats.diff_apply_count;
      }
    } else if (root != nullptr) {
      applyWidgetData(root, view);
      ++stats.diff_apply_count;
    }
    prev_data = std::move(new_data);

    // Open sub-dialog if requested (read-only modal popup, no event plumbing).
    if (sub_dialog_ui.has_value() && root != nullptr) {
      QByteArray sub_data(sub_dialog_ui->data(), static_cast<int>(sub_dialog_ui->size()));
      QBuffer sub_buffer(&sub_data);
      sub_buffer.open(QIODevice::ReadOnly);
      PjUiLoader sub_loader;
      QWidget* sub_loaded = sub_loader.load(&sub_buffer, root);
      if (sub_loaded != nullptr) {
        QDialog* sub_dialog = qobject_cast<QDialog*>(sub_loaded);
        if (sub_dialog == nullptr) {
          sub_dialog = new QDialog(root);
          auto* sub_layout = new QVBoxLayout(sub_dialog);
          sub_layout->setContentsMargins(0, 0, 0, 0);
          sub_layout->addWidget(sub_loaded);
        }
        // "[*]" renders empty yet stops Qt appending the " — PlotJuggler 4" title suffix.
        sub_dialog->setWindowTitle(sub_loaded->windowTitle() + "[*]");
        // Frameless + theme-painted, like the app's own dialogs. The .ui ships a
        // plain QDialog that otherwise gets the native OS titlebar (which doesn't
        // match the dark/light chrome). FramelessWindowHint drops the OS frame;
        // WA_StyledBackground lets the global `QDialog { background: ... }` QSS
        // paint the themed surface. A 1px border gives the borderless window
        // definition against whatever sits behind it.
        sub_dialog->setWindowFlag(Qt::FramelessWindowHint, true);
        sub_dialog->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        sub_dialog->setAttribute(Qt::WA_StyledBackground, true);
        sub_dialog->setStyleSheet(
            sub_dialog->styleSheet() + QStringLiteral("\nQDialog{border:1px solid palette(mid);}"));
        // Wire the standard QDialogButtonBox (objectName "buttonBox") to
        // QDialog::accept/reject. Without this the OK/Cancel buttons are
        // inert and the only way to close the sub-dialog is the window
        // manager's X — the OK click would do nothing.
        if (auto* button_box = sub_dialog->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))) {
          QObject::connect(button_box, &QDialogButtonBox::accepted, sub_dialog, &QDialog::accept);
          QObject::connect(button_box, &QDialogButtonBox::rejected, sub_dialog, &QDialog::reject);
        }
        // Pre-fill the sub-dialog from the current widget data so it opens
        // populated. Names that don't exist in the sub-dialog are simply
        // skipped, so the panel's own widget values don't leak in.
        applyWidgetData(sub_dialog, view);
        // exec() spins a nested modal event loop. Pause our tick timer for its
        // duration so a timer-driven applyAndDiff() can't re-enter on the same
        // Impl while the sub-dialog is open — a re-entrant tick could observe a
        // __request_close and tear us down (and delete sub_dialog) underneath
        // the suspended outer call. Resume only if we weren't closed meanwhile.
        const bool timer_was_active = (tick_timer != nullptr) && tick_timer->isActive();
        if (timer_was_active) {
          tick_timer->stop();
        }
        const int dlg_result = sub_dialog->exec();
        if (timer_was_active && !closed && tick_timer != nullptr) {
          tick_timer->start();
        }
        // When the user clicks OK, harvest the values the user typed
        // into the sub-dialog's inputs and surface them to the plugin
        // through the existing event channels. Plugins that want
        // user input from a sub-dialog override `onTextChanged` /
        // `onClicked` for the dialog's widget names (each must have
        // an objectName set in the .ui), then handle the synthetic
        // `subDialogAccepted` click as the "OK was pressed, commit
        // changes" signal. Without this loop, the values typed into
        // the sub-dialog are dropped on the floor.
        if (dlg_result == QDialog::Accepted) {
          for (auto* line_edit : sub_dialog->findChildren<QLineEdit*>()) {
            const QString name = line_edit->objectName();
            if (name.isEmpty() || name.startsWith(QStringLiteral("qt_"))) {
              continue;
            }
            nlohmann::json ev = {{"text", line_edit->text().toStdString()}};
            (void)handle.sendEvent(name.toStdString(), ev.dump());
          }
          for (auto* check_box : sub_dialog->findChildren<QCheckBox*>()) {
            const QString name = check_box->objectName();
            if (name.isEmpty() || name.startsWith(QStringLiteral("qt_"))) {
              continue;
            }
            nlohmann::json ev = {{"checked", check_box->isChecked()}};
            (void)handle.sendEvent(name.toStdString(), ev.dump());
          }
          for (auto* combo_box : sub_dialog->findChildren<QComboBox*>()) {
            const QString name = combo_box->objectName();
            if (name.isEmpty() || name.startsWith(QStringLiteral("qt_"))) {
              continue;
            }
            nlohmann::json ev = {
                {"current_text", combo_box->currentText().toStdString()}, {"current_index", combo_box->currentIndex()}};
            (void)handle.sendEvent(name.toStdString(), ev.dump());
          }
          (void)handle.sendEvent("subDialogAccepted", R"({"clicked":true})");
        }
        delete sub_dialog;
      }
    }

    return close_reason;
  }
};

PanelEngine::PanelEngine(DialogHandle handle, PanelEngineConfig config, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(std::move(handle), config)) {}

PanelEngine::~PanelEngine() {
  close();
}

QWidget* PanelEngine::openPanel() {
  impl_->stats = {};

  // 1. Load the .ui blob into a QWidget.
  std::string ui = impl_->handle.ui_content();
  if (ui.empty()) {
    return nullptr;
  }
  QByteArray data(ui.data(), static_cast<int>(ui.size()));
  QBuffer buffer(&data);
  buffer.open(QIODevice::ReadOnly);

  PjUiLoader loader;
  QWidget* loaded = loader.load(&buffer);
  if (loaded == nullptr) {
    return nullptr;
  }
  impl_->root = loaded;

  // 2. Apply initial widget data.
  std::string initial_raw = impl_->handle.widget_data();
  if (!initial_raw.empty()) {
    nlohmann::json initial_data = nlohmann::json::parse(initial_raw, nullptr, false);
    if (!initial_data.is_discarded()) {
      // Strip one-shot commands so they don't get re-applied on every tick.
      initial_data.erase("__request_close");
      initial_data.erase("__request_sub_dialog");
      initial_data.erase("__request_accept");
      WidgetDataView view(initial_raw);
      applyWidgetData(loaded, view);
      impl_->prev_data = std::move(initial_data);
      impl_->prev_raw = initial_raw;
    }
  }

  // 3. Wire widget signals to forward events into the plugin.
  connectWidgetSignals(loaded, [this](const std::string& name, const std::string& event_json) {
    if (impl_->closed) {
      return;
    }
    ++impl_->stats.event_count;
    if (impl_->handle.sendEvent(name, event_json)) {
      if (auto reason = impl_->applyAndDiff(); reason.has_value()) {
        if (impl_->close_cb) {
          impl_->close_cb(*reason);
        }
        this->close();
      }
    }
  });

  // 3b. Wire the panel's standard QDialogButtonBox (objectName "buttonBox").
  // connectWidgetSignals deliberately skips buttons owned by a button box, and
  // unlike a modal dialog the non-modal panel has no QDialog::accept/reject to
  // fall back on — so without this the Close/OK buttons are inert (the reported
  // bug: Close does nothing in every toolbox). Route them through the same
  // close path as a plugin-requested __request_close.
  if (auto* button_box = loaded->findChild<QDialogButtonBox*>(QStringLiteral("buttonBox"))) {
    auto on_close = [this]() {
      if (impl_->closed) {
        return;
      }
      if (impl_->close_cb) {
        impl_->close_cb("closed by user");
      }
      this->close();
    };
    QObject::connect(button_box, &QDialogButtonBox::rejected, this, on_close);
    QObject::connect(button_box, &QDialogButtonBox::accepted, this, on_close);
  }

  // 4. Install drop event filter for any declared drop targets.
  {
    WidgetDataView drop_view(initial_raw);
    auto targets = drop_view.dropTargets();
    if (!targets.empty()) {
      auto* drop_filter = new DropEventFilter(loaded, [this](const std::string& name, const std::string& event_json) {
        if (impl_->closed) {
          return;
        }
        ++impl_->stats.event_count;
        if (impl_->handle.sendEvent(name, event_json)) {
          if (auto reason = impl_->applyAndDiff(); reason.has_value()) {
            if (impl_->close_cb) {
              impl_->close_cb(*reason);
            }
            this->close();
          }
        }
      });
      for (const auto& t : targets) {
        drop_filter->addTarget(t);
      }
      if (impl_->config.catalog_key_resolver) {
        drop_filter->setKeyResolver(impl_->config.catalog_key_resolver);
      }
    }
  }

  // 5. Install keyboard shortcuts declared in widget data.
  {
    WidgetDataView shortcut_view(impl_->handle.widget_data());
    installButtonShortcuts(loaded, shortcut_view);
  }

  // 6. Start tick timer.
  //
  // PanelEngine semantics differ from DialogEngine here: every tick polls
  // widget_data() unconditionally, regardless of whether the plugin's
  // on_tick reports state change. Long-lived panels frequently have their
  // state updated by external sources (worker threads, async fetch
  // callbacks) that the plugin's on_tick cannot observe. on_tick is still
  // called so the plugin can advance any internal periodic logic.
  impl_->tick_timer = new QTimer(this);
  impl_->tick_timer->setInterval(impl_->config.tick_interval_ms);
  QObject::connect(impl_->tick_timer, &QTimer::timeout, this, [this]() {
    if (impl_->closed) {
      return;
    }
    ++impl_->stats.tick_count;
    (void)impl_->handle.tick();
    if (auto reason = impl_->applyAndDiff(); reason.has_value()) {
      if (impl_->close_cb) {
        impl_->close_cb(*reason);
      }
      this->close();
    }
  });
  impl_->tick_timer->start();

  return loaded;
}

void PanelEngine::close() {
  if (impl_->closed) {
    return;
  }
  impl_->closed = true;
  if (impl_->tick_timer != nullptr) {
    impl_->tick_timer->stop();
  }
  impl_->handle.reject();
}

void PanelEngine::onCloseRequested(std::function<void(std::string)> cb) {
  impl_->close_cb = std::move(cb);
}

PanelEngine::Stats PanelEngine::stats() const {
  return impl_->stats;
}

}  // namespace PJ
