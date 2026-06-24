#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

#include "pj_base/types.hpp"
#include "pj_widgets/Timeline.h"

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace PJ {

class AppSession;
class Timeline;

/// Binds a runtime-agnostic Timeline widget to the AppSession services: builds
/// tracks from the catalog + per-source ranges + offsets, applies the widget's
/// intent signals to SessionManager/PlaybackEngine, and pushes runtime state
/// back into the widget. The ONLY place that knows both the widget and
/// pj_runtime — the widget itself never includes a pj_runtime header.
class SourceTimelineController : public QObject {
  Q_OBJECT
 public:
  SourceTimelineController(Timeline* widget, AppSession* session, QObject* parent = nullptr);

  /// Current top-to-bottom track order, as DatasetIds, exactly as the bars are
  /// drawn (drag order honored, remaining datasets in catalog order). Used by
  /// layout-save to persist the timeline's vertical arrangement.
  [[nodiscard]] std::vector<DatasetId> currentTrackOrder() const;

  /// Restore a persisted top-to-bottom track order (from a loaded layout) and
  /// rebuild. `order` lists the DatasetIds front-to-back; ids not present fall
  /// back to catalog order, exactly as a live drag-reorder would. Pass the ids
  /// of the just-reloaded datasets, mapped from the layout's saved order.
  void setDisplayOrder(std::vector<DatasetId> order);

  /// Record `anchor` as a merge result so its bar paints in the distinct "merged"
  /// color, then rebuild. Called after a merge confirmed from EITHER entry point
  /// (the timeline footer or the curve tree's context menu via MainWindow).
  void markDatasetMerged(DatasetId anchor);

 public slots:
  /// Align every source's displayed START to the earliest start (the in-strip
  /// "Align" behaviour, now driven from the timeline panel's align rail).
  void alignStarts();

  /// Align every source's displayed CENTER to the earliest center.
  void alignCenters();

  /// Align every source's displayed END to the latest end.
  void alignEnds();

  /// Undo all timeline edits for every source: zero each display offset and drop
  /// the user's drag order back to catalog order. Driven by the align rail's reset.
  void resetAll();

  /// Set (or clear) the blue reference needle from a PLAYBACK-frame position. The
  /// Timeline itself bridges into its bar frame (via the integer-ns time-frame
  /// offset the controller keeps in sync), so this passes the value straight
  /// through. nullopt hides the needle. Called by MainWindow, which owns the toggle.
  void setReferenceLine(std::optional<double> playback_seconds);

 private slots:
  /// Repopulate the widget's tracks from the current catalog/ranges/offsets.
  /// Cheap and idempotent; driven by catalog changes (add/remove/clear/reorder).
  void rebuildTracks();

  /// Confirm (destructive) then merge the selected datasets via the shared
  /// confirmAndMergeDatasets helper, marking the surviving anchor on success.
  void onMergeRequested(const QList<quint64>& ids);

 private:
  /// Refresh only the cached tracks' display offsets from the SessionManager and
  /// re-feed them to the widget. The offset-only counterpart to rebuildTracks():
  /// the raw ranges/names/colors/order are unchanged, so it skips the per-dataset
  /// datasetRawTimeRange catalog scan on the hot live-drag path.
  void updateTrackOffsets();

  /// Shared body of alignStarts()/alignCenters(): run the named alignment over
  /// the controller's current track list via the core engine, then write each
  /// resulting offset back through the SessionManager.
  enum class AlignMode : std::uint8_t { kStarts, kCenters, kEnds };
  void applyAlignment(AlignMode mode);

  /// Schedule a coalesced (single-shot ~16 ms) playback-range recompute. Live
  /// drags fire many offset writes; this batches them into one recompute.
  void scheduleRangeRecompute();

  /// Stable palette color for a dataset, assigned on first sight in insertion
  /// order and remembered thereafter.
  [[nodiscard]] QColor colorFor(DatasetId id);

  Timeline* widget_;
  AppSession* session_;
  QTimer* range_recompute_timer_;             // coalesces recomputeRange() during drags
  QHash<DatasetId, QColor> colors_;           // stable per-source palette assignment
  std::vector<TimelineTrack> tracks_;         // last built track list (drives align)
  std::unordered_set<DatasetId> merged_ids_;  // datasets produced by a merge (distinct bar color)
  // User-chosen top-to-bottom track order from a name-column drag (Timeline::
  // tracksReordered) or restored from a layout (setDisplayOrder). rebuildTracks()
  // honors it; ids not present fall back to catalog (load) order. Persisted to
  // layouts (per-source timeline_order) and re-bound by source path on reload.
  std::vector<DatasetId> display_order_;
};

}  // namespace PJ
