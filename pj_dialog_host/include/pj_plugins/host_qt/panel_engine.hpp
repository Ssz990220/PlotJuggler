// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <QObject>
#include <QWidget>
#include <functional>
#include <memory>
#include <pj_plugins/host/dialog_handle.hpp>
#include <string>

namespace PJ {

/// Configuration for PanelEngine.
struct PanelEngineConfig {
  int tick_interval_ms = 50;
  bool enable_diff = true;

  /// Optional resolver from a dragged catalog key (e.g.
  /// "dataset:1/topic:1/column:2") to a human field name (e.g. "topic/field")
  /// before the drop is delivered to the plugin's onItemsDropped. The PJ4 curve
  /// tree drags opaque catalog keys; plugins (Quaternion, FFT, …) expect names.
  /// If unset, or if it returns empty for a key, that key is delivered verbatim.
  std::function<std::string(const std::string& catalog_key)> catalog_key_resolver;
};

/// Hosts a long-lived interactive panel built from a plugin's typed-dialog UI.
///
/// Sibling of DialogEngine. Same .ui loader, same widget binding, same
/// tick-and-diff mechanism. Differences:
///   * Returns a bare QWidget* via openPanel() instead of running QDialog::exec().
///   * No required QDialogButtonBox — the plugin draws its own button row.
///   * Close is plugin-initiated via WidgetData::requestClose("<reason>");
///     the engine forwards the reason via the callback set with
///     onCloseRequested() and then tears down the panel.
///
/// Typical usage from pj_app:
///   auto* engine = new PJ::PanelEngine(std::move(dialog_handle), {}, this);
///   engine->onCloseRequested([this](std::string reason) { restoreCentralArea(); });
///   QWidget* widget = engine->openPanel();
///   if (widget == nullptr) { ... handle error ... }
///   mainWindow->presentPanel(widget);  // takes ownership; engine keeps a weak ref
class PanelEngine : public QObject {
  Q_OBJECT
 public:
  explicit PanelEngine(DialogHandle handle, PanelEngineConfig config = {}, QObject* parent = nullptr);
  ~PanelEngine() override;

  PanelEngine(const PanelEngine&) = delete;
  PanelEngine& operator=(const PanelEngine&) = delete;
  PanelEngine(PanelEngine&&) = delete;
  PanelEngine& operator=(PanelEngine&&) = delete;

  /// Build the QWidget from the plugin's .ui blob, apply initial widget data,
  /// wire widget signals, start the tick timer. Returns a non-owning pointer
  /// to the constructed widget — the caller parents it into its target layout
  /// (typically MainWindow's central area). PanelEngine retains the widget
  /// reference for tick/event delivery; deletion of the widget (e.g. by
  /// re-parenting and dropping) does not crash the engine but stops further
  /// updates.
  ///
  /// Returns nullptr on .ui load failure.
  [[nodiscard]] QWidget* openPanel();

  /// Stop the tick timer and call the plugin's on_rejected. Safe to call
  /// multiple times; idempotent.
  void close();

  /// Set the callback fired when the plugin emits requestClose("<reason>").
  /// The string carries the plugin-provided reason. After the callback runs,
  /// PanelEngine calls close() on itself.
  void onCloseRequested(std::function<void(std::string /*reason*/)> cb);

  /// Statistics from the current panel session (zeroed on each openPanel).
  struct Stats {
    int tick_count = 0;
    int event_count = 0;
    int diff_apply_count = 0;
  };
  [[nodiscard]] Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
