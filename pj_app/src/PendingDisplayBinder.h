// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <optional>
#include <utility>
#include <vector>

#include "LayoutXml.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_scene_common/scene_dock_widget.h"

namespace PJ {

class TopicDemandTracker;

// One staged display intent waiting for its topic to materialize: either a plot
// curve (kCurve — from a layout restore or a placeholder drop on a plot) or a
// scene layer (kSceneLayer — a placeholder drop on a scene dock, completed once
// the real ObjectTopicId exists).
struct PendingDisplayEntry {
  enum class Kind { kCurve, kSceneLayer };

  Kind kind = Kind::kCurve;
  QPointer<PlotWidget> plot;             ///< kCurve target; null means the plot was destroyed.
  QPointer<SceneDockWidget> scene_dock;  ///< kSceneLayer target; null means the dock was destroyed.
  layout_xml::SeriesPath path;           ///< Topic (+field for curves; XY: the Y source).
  layout_xml::SeriesPath x_path;         ///< Non-empty topic means this curve entry is XY.
  QDomDocument curve_doc;                ///< kCurve: owns the detached <curve> clone.
  QDomElement curve_element;             ///< kCurve: element imported into curve_doc.
  // Demand references held for this entry (see PendingDisplayBinder's class doc):
  // one per (dataset, topic name) the entry's path(s) currently name. Empty when
  // no tracker was supplied to the binder.
  std::vector<std::pair<DatasetId, QString>> demand_refs;
  // The dataset the entry was staged against (an interactive drop knows its
  // placeholder's dataset; layout-restore entries don't). Resolution prefers
  // this dataset when two datasets name the same topic, falling back to the
  // load-order scan if it vanished (stream reconnects mint fresh ids).
  std::optional<DatasetId> preferred_dataset;

  [[nodiscard]] bool isXY() const {
    return kind == Kind::kCurve && !x_path.topic.isEmpty();
  }

  /// True once the widget this entry completes against has been destroyed.
  [[nodiscard]] bool targetIsNull() const {
    return kind == Kind::kCurve ? plot.isNull() : scene_dock.isNull();
  }
};

// Resolves a stable layout SeriesPath to a concrete catalog key, never guessing by
// load order. Thin adapter that unpacks the SeriesPath's qualifiers into
// CatalogModel::resolveCurveKey, which owns the shared three-tier algorithm (exact
// id while qualifiers agree → unique full-path match → unique raw-source match, then
// confirm topic+field; unqualified path binds only when topic+field is globally
// unique). Kept as a free function so layout/undo restore call sites read the same
// as before; see resolveCurveKey for the full resolution + ambiguity semantics.
[[nodiscard]] std::optional<QString> resolveSeriesPath(const CatalogModel& catalog, const layout_xml::SeriesPath& path);

// GUI-thread-only registry for display intents whose topics were not in the catalog
// when they were staged: plot curves from a progressive layout restore (collect()) or
// a placeholder drop on a plot (addPendingCurve()), and scene layers from a
// placeholder drop on a scene dock (addPendingSceneLayer()). It owns no widgets; a
// target widget's destruction releases its entries (and their references) eagerly —
// a dead pend must not keep its topic subscribed while the stream stays quiet.
//
// Pending-binding-is-a-reference invariant: a demand-capable streaming source only transmits a
// topic while something references it (see TopicDemandTracker); a pending intent IS such a
// reference, or the topic this entry is waiting on would never subscribe and the entry would
// wait forever. So every entry registers a reference for its topic(s) when staged and releases
// it exactly once — on successful completion, on the target widget's destruction, or on
// clear()/a fresh collect(). Pass `tracker` (optional; nullptr = no demand tracking, e.g.
// plain unit tests).
class PendingDisplayBinder : public QObject {
 public:
  explicit PendingDisplayBinder(CatalogModel& catalog, TopicDemandTracker* tracker = nullptr);

  // Re-walks the saved layout and stores unresolved per-plot curves as detached DOM clones.
  // Replaces ALL previously staged entries — scene pends too — releasing their demand
  // references first (a layout load rebuilds the dock world, so staged intents against
  // the old widgets are moot).
  void collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id);

  // Stages ONE interactive drop (M3-UI: dropping an advertised placeholder scalar
  // topic onto `plot`) without disturbing any other staged entry. `path.field`
  // empty names "whatever scalar fields the topic turns out to have" — a
  // placeholder carries no field breakdown before real data arrives, so when the
  // topic materializes the entry binds one curve per scalar field (mirroring a
  // normal topic-node drop). `preferred_dataset` scopes resolution when two
  // datasets advertise the same topic name.
  void addPendingCurve(
      PlotWidget* plot, const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred_dataset = std::nullopt);

  // Stages ONE placeholder drop on a scene dock: completes via dock->addTopic()
  // once an object topic with this name materializes (preferring
  // `preferred_dataset`, falling back to any dataset naming it — stream
  // reconnects mint fresh ids). The entry is consumed when the topic
  // materializes even if the dock declines it, mirroring an on-arrival drop.
  void addPendingSceneLayer(
      SceneDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset);

  // Attempts pending entries whose topics arrived; an empty set is the drain pass and tries all entries.
  [[nodiscard]] int flush(const QSet<QString>& topics);

  // Remaining unresolved stable paths for a final missing-curve prompt; dead plots
  // and scene entries are omitted (the prompt is layout-restore-scoped, and scene
  // pends only come from interactive drops).
  [[nodiscard]] QList<layout_xml::SeriesPath> unresolved() const;

  void clear();
  [[nodiscard]] bool empty() const;
  [[nodiscard]] int size() const;

 private:
  // Ensures a demand reference exists for every dataset that currently names
  // entry.path's topic (real or advertised placeholder — the "safe superset" the
  // class doc calls for), and entry.x_path's topic too for an XY entry.
  // Idempotent, so it runs at stage time AND on every flush() pass: a layout
  // restored before its stream connects stages entries while no dataset names
  // the topic yet, and the reference must attach when the advertise burst
  // lands. No-op without a tracker.
  void refreshDemandRefs(PendingDisplayEntry& entry);
  // Releases exactly the references refreshDemandRefs recorded for this entry.
  void releaseDemandRefs(const PendingDisplayEntry& entry);
  // resolveSeriesPath honoring `preferred`: exact match in that dataset first,
  // then the global load-order scan (the preferred id may have vanished — stream
  // reconnects mint fresh dataset ids).
  [[nodiscard]] std::optional<QString> resolveEntryPath(
      const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const;
  // Keys of every scalar field materialized for `topic`, all from one dataset
  // (`preferred` when it has any, else the first dataset naming the topic) — the
  // empty-field placeholder-drop fallback binds one curve per returned key.
  [[nodiscard]] std::vector<QString> scalarKeysForTopic(const QString& topic, std::optional<DatasetId> preferred) const;
  // First object topic naming `topic` (preferring `preferred`), or nullopt while
  // none has materialized — the kSceneLayer twin of resolveEntryPath.
  [[nodiscard]] std::optional<CatalogItem> resolveObjectTopic(
      const QString& topic, std::optional<DatasetId> preferred) const;
  // Completes one kSceneLayer entry if its object topic materialized. True means
  // the entry is consumed (release refs + erase), false means keep waiting.
  [[nodiscard]] bool tryCompleteSceneEntry(PendingDisplayEntry& entry);
  // True when an identical intent (same kind, same live target widget, same
  // topic/field path) is already staged — the dedup gate for interactive drops.
  [[nodiscard]] bool hasEntryFor(
      PendingDisplayEntry::Kind kind, const QObject* target, const layout_xml::SeriesPath& path) const;
  // Releases entries (and their references) the moment their target widget is
  // destroyed — connected once per distinct target at stage time. Lazy QPointer
  // checks are not enough: on a quiet stream no flush ever runs, and a dead
  // pend's reference would keep its topic subscribed forever.
  void watchTargetDestruction(QObject* target);

  CatalogModel& catalog_;
  TopicDemandTracker* tracker_ = nullptr;
  std::vector<PendingDisplayEntry> entries_;
  // Targets with a live destroyed-hook; entries may outnumber targets (several
  // pends on one plot) and a stale hook after the last entry completes is
  // harmless (it finds nothing to release).
  QSet<QObject*> watched_targets_;
};

}  // namespace PJ
