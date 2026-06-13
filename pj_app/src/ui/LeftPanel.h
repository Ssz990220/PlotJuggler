#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDomDocument>
#include <QDomElement>
#include <QWidget>
#include <vector>

#include "pj_plugins/host/plugin_runtime_catalog.hpp"
#include "pj_widgets/ChromeMetrics.h"

namespace Ui {
class LeftPanel;
}

namespace PJ {

// "Sources" section: file load + streaming source. Emits high-level
// user-intent signals — MainWindow wires them to services.
class LeftPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LeftPanel(QWidget* parent = nullptr);
  ~LeftPanel() override;

 signals:
  void loadDataRequested();
  void reloadDataRequested();
  // Emitted when the user picks a path from the recent-files menu in
  // the Input header. MainWindow connects this to FileLoader::loadFile.
  void recentFileSelected(QString path);
  // The cog button is a one-shot Start action — there is no "stop" affordance
  // in the UI (Davide: streaming should always be open). Emitted on click.
  void streamingStartRequested();
  // Pause/resume of the active stream session(s). When paused, samples keep
  // ingesting but the viewport no longer follows the live edge — mirrors PJ3
  // semantics. Wired into StreamingSourceManager::onPauseToggled.
  void streamingPauseToggled(bool paused);
  void streamingSourceChanged(QString source);
  // Buffer length (seconds) for the streaming source. Persisted to
  // QSettings; emitted when the user adjusts the inline scrubber.
  void streamingBufferChanged(int seconds);
  // Emitted when the user clicks a cloud-tagged toolbox entry in the Cloud
  // page. `plugin_id` is the toolbox's manifest `id`; MainWindow wires this to
  // the launcher slot that binds a ToolboxRuntimeHost and presents the panel.
  void cloudToolboxRequested(QString plugin_id);

 public slots:
  void onStylesheetChanged(QString theme);
  // Rebinds Chrome metrics from MainWindow. Re-runs applyIcons() so
  // the Sources band, page rows, and streaming row absorb new icon
  // metrics, layout padding and the spacing between items.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);
  // Repopulates the streaming combo. Preserves the current selection if the
  // previously-selected name is still present.
  void setStreamingSources(const QStringList& names);
  void setReloadEnabled(bool enabled);
  void setRecentEnabled(bool enabled);

  // Builds <left_panel_state sources_tab="..." streaming_source="..."
  // streaming_buffer="..."/>. Caller appends to the layout document.
  // visibility is NOT included here — MainWindow handles it via chrome_state.
  [[nodiscard]] QDomElement saveSourcesState(QDomDocument& doc) const;

  // Applies <left_panel_state> attributes individually; missing or
  // mismatched values are silently ignored. Never writes to QSettings —
  // layout-driven UI changes don't mutate the global per-user defaults.
  void restoreSourcesState(const QDomElement& element);

 public:
  // Rebuilds the Cloud page from the catalog. Not a slot: RuntimeToolboxPlugin
  // is non-copyable (owns a ToolboxLibrary), so MOC can't marshal it. Filters
  // to toolboxes whose manifest `tags` contains "cloud" and renders one button
  // per match. Safe to call repeatedly (e.g. on catalogChanged).
  void populateCloudToolboxes(const std::vector<RuntimeToolboxPlugin>& toolboxes);

 private:
  void applyIcons(QString theme);
  // Swaps the pause/resume icon and tooltip to match the button's checked
  // state. Called from applyIcons() and on every toggled() emission so the
  // glyph tracks both theme changes and user clicks.
  void applyPauseButtonState(QString theme);

  Ui::LeftPanel* ui_;
  // Chrome metrics from MainWindow::chromeMetricsChanged.
  ChromeMetrics chrome_metrics_;
};

}  // namespace PJ
