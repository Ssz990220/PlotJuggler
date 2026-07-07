// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PendingDisplayBinder.h"

#include <QDomNodeList>
#include <algorithm>
#include <utility>

#include "pj_runtime/TopicDemandTracker.h"

namespace PJ {

namespace {
// Every dataset that currently names `topic_name` in the catalog — real data or
// an advertised placeholder both count (see PendingDisplayBinder's class doc: a
// pending bind is a demand reference, and referencing every candidate dataset is
// the safe superset when the name is ambiguous across datasets).
std::vector<DatasetId> datasetsNaming(const CatalogModel& catalog, const QString& topic_name) {
  std::vector<DatasetId> ids;
  if (topic_name.isEmpty()) {
    return ids;
  }
  for (const CatalogItem& item : catalog.items()) {
    if (item.topic_name == topic_name && std::find(ids.begin(), ids.end(), item.dataset_id) == ids.end()) {
      ids.push_back(item.dataset_id);
    }
  }
  return ids;
}
}  // namespace

std::optional<QString> resolveSeriesPath(const CatalogModel& catalog, const layout_xml::SeriesPath& path) {
  for (const auto& [id, name] : catalog.datasets()) {
    (void)name;
    if (const auto descriptor = catalog.descriptorForPath(id, path.topic, path.field)) {
      return descriptor->name;
    }
  }
  return std::nullopt;
}

PendingDisplayBinder::PendingDisplayBinder(CatalogModel& catalog, TopicDemandTracker* tracker)
    : catalog_(catalog), tracker_(tracker) {}

void PendingDisplayBinder::refreshDemandRefs(PendingDisplayEntry& entry) {
  if (tracker_ == nullptr) {
    return;
  }
  // Idempotent: only datasets not yet referenced by this entry gain a reference.
  // Called at stage time AND on every flush() pass, because the topic may become
  // nameable only later — a layout restored before its stream connects stages
  // entries while NO dataset names the topic yet; the reference must attach the
  // moment the advertise burst lands, or the topic never subscribes and the
  // entry waits forever.
  const auto ensure_refs_for = [this, &entry](const QString& topic_name) {
    for (const DatasetId id : datasetsNaming(catalog_, topic_name)) {
      const auto ref = std::make_pair(id, topic_name);
      if (std::find(entry.demand_refs.begin(), entry.demand_refs.end(), ref) == entry.demand_refs.end()) {
        tracker_->addReference(id, topic_name);
        entry.demand_refs.push_back(ref);
      }
    }
  };
  ensure_refs_for(entry.path.topic);
  if (entry.isXY()) {
    ensure_refs_for(entry.x_path.topic);
  }
}

void PendingDisplayBinder::releaseDemandRefs(const PendingDisplayEntry& entry) {
  if (tracker_ == nullptr) {
    return;
  }
  for (const auto& [id, name] : entry.demand_refs) {
    tracker_->removeReference(id, name);
  }
}

void PendingDisplayBinder::collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id) {
  for (const PendingDisplayEntry& entry : entries_) {
    releaseDemandRefs(entry);
  }
  entries_.clear();

  const QDomNodeList plot_nodes = doc.elementsByTagName(QStringLiteral("plot"));
  for (int i = 0; i < plot_nodes.size(); ++i) {
    const QDomElement plot_element = plot_nodes.at(i).toElement();
    if (plot_element.isNull()) {
      continue;
    }
    PlotWidget* plot = plots_by_state_id.value(plot_element.attribute(QStringLiteral("id")), nullptr);
    if (plot == nullptr) {
      continue;
    }

    for (QDomElement curve = plot_element.firstChildElement(QStringLiteral("curve")); !curve.isNull();
         curve = curve.nextSiblingElement(QStringLiteral("curve"))) {
      PendingDisplayEntry entry;
      entry.plot = plot;

      if (curve.hasAttribute(QStringLiteral("x_topic"))) {
        entry.x_path = layout_xml::SeriesPath{
            curve.attribute(QStringLiteral("x_topic")),
            curve.attribute(QStringLiteral("x_field")),
        };
        entry.path = layout_xml::SeriesPath{
            curve.attribute(QStringLiteral("y_topic")),
            curve.attribute(QStringLiteral("y_field")),
        };
        if (resolveSeriesPath(catalog_, entry.x_path).has_value() &&
            resolveSeriesPath(catalog_, entry.path).has_value()) {
          continue;
        }
      } else if (curve.hasAttribute(QStringLiteral("topic"))) {
        entry.path =
            layout_xml::SeriesPath{curve.attribute(QStringLiteral("topic")), curve.attribute(QStringLiteral("field"))};
        if (resolveSeriesPath(catalog_, entry.path).has_value()) {
          continue;
        }
      } else {
        continue;
      }

      entry.curve_element = entry.curve_doc.importNode(curve, /*deep=*/true).toElement();
      entry.curve_doc.appendChild(entry.curve_element);
      refreshDemandRefs(entry);
      watchTargetDestruction(plot);
      entries_.push_back(std::move(entry));
    }
  }
}

bool PendingDisplayBinder::hasEntryFor(
    PendingDisplayEntry::Kind kind, const QObject* target, const layout_xml::SeriesPath& path) const {
  for (const PendingDisplayEntry& entry : entries_) {
    const QObject* entry_target = entry.kind == PendingDisplayEntry::Kind::kCurve
                                      ? static_cast<const QObject*>(entry.plot.data())
                                      : static_cast<const QObject*>(entry.scene_dock.data());
    if (entry.kind == kind && entry_target == target && entry.path.topic == path.topic &&
        entry.path.field == path.field) {
      return true;
    }
  }
  return false;
}

void PendingDisplayBinder::addPendingCurve(
    PlotWidget* plot, const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred_dataset) {
  if (plot == nullptr || path.topic.isEmpty()) {
    return;
  }
  // Double-dropping the same placeholder must not stage a second intent: on
  // promotion the first entry binds every field, the duplicate's addCurve()
  // calls all return null, and it would wait forever holding its demand
  // reference (and re-add ghost curves after a manual delete on a later flush).
  if (hasEntryFor(PendingDisplayEntry::Kind::kCurve, plot, path)) {
    return;
  }
  PendingDisplayEntry entry;
  entry.plot = plot;
  entry.path = path;
  entry.preferred_dataset = preferred_dataset;
  entry.curve_element = entry.curve_doc.createElement(QStringLiteral("curve"));
  entry.curve_doc.appendChild(entry.curve_element);
  entry.curve_element.setAttribute(QStringLiteral("topic"), path.topic);
  entry.curve_element.setAttribute(QStringLiteral("field"), path.field);
  refreshDemandRefs(entry);
  watchTargetDestruction(plot);
  entries_.push_back(std::move(entry));
}

void PendingDisplayBinder::addPendingSceneLayer(
    SceneDockWidget* dock, const QString& topic_name, std::optional<DatasetId> preferred_dataset) {
  if (dock == nullptr || topic_name.isEmpty()) {
    return;
  }
  if (hasEntryFor(PendingDisplayEntry::Kind::kSceneLayer, dock, layout_xml::SeriesPath{topic_name, QString()})) {
    return;  // same dock, same topic — one pending layer is enough
  }
  PendingDisplayEntry entry;
  entry.kind = PendingDisplayEntry::Kind::kSceneLayer;
  entry.scene_dock = dock;
  entry.path = layout_xml::SeriesPath{topic_name, QString()};
  entry.preferred_dataset = preferred_dataset;
  refreshDemandRefs(entry);
  watchTargetDestruction(dock);
  entries_.push_back(std::move(entry));
}

void PendingDisplayBinder::watchTargetDestruction(QObject* target) {
  if (watched_targets_.contains(target)) {
    return;
  }
  watched_targets_.insert(target);
  connect(target, &QObject::destroyed, this, [this](QObject* dead) {
    watched_targets_.remove(dead);
    // Match the dying widget by raw pointer: for QWidget-derived targets the
    // QPointer is still VALID inside destroyed() (it nulls only for the QObject
    // dtor layer), so a targetIsNull() sweep would find nothing here. Entries
    // already nulled by an earlier death are swept too.
    auto it = entries_.begin();
    while (it != entries_.end()) {
      const bool owned_by_dead =
          static_cast<QObject*>(it->plot.data()) == dead || static_cast<QObject*>(it->scene_dock.data()) == dead;
      if (owned_by_dead || it->targetIsNull()) {
        releaseDemandRefs(*it);
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  });
}

std::optional<CatalogItem> PendingDisplayBinder::resolveObjectTopic(
    const QString& topic, std::optional<DatasetId> preferred) const {
  std::optional<CatalogItem> fallback;
  for (const CatalogItem& item : catalog_.items()) {
    if (item.topic_name != topic || asObjectTopic(item) == nullptr) {
      continue;
    }
    if (preferred.has_value() && item.dataset_id == *preferred) {
      return item;
    }
    if (!fallback.has_value()) {
      fallback = item;
    }
  }
  return fallback;
}

bool PendingDisplayBinder::tryCompleteSceneEntry(PendingDisplayEntry& entry) {
  const std::optional<CatalogItem> item = resolveObjectTopic(entry.path.topic, entry.preferred_dataset);
  if (!item.has_value()) {
    return false;
  }
  const auto* object_topic = asObjectTopic(*item);
  // addTopic fires layerAdded, whose handler re-establishes the displayed
  // reference BEFORE the caller releases this entry's pend hold — the topic
  // never transiently looks unreferenced. A dock that declines the topic still
  // consumes the entry, mirroring what an on-arrival drop would have done.
  static_cast<void>(
      entry.scene_dock->addTopic(object_topic->object_topic_id, object_topic->object_type, item->topic_name));
  return true;
}

std::optional<QString> PendingDisplayBinder::resolveEntryPath(
    const layout_xml::SeriesPath& path, std::optional<DatasetId> preferred) const {
  if (preferred.has_value()) {
    if (const auto descriptor = catalog_.descriptorForPath(*preferred, path.topic, path.field)) {
      return descriptor->name;
    }
  }
  return resolveSeriesPath(catalog_, path);
}

std::vector<QString> PendingDisplayBinder::scalarKeysForTopic(
    const QString& topic, std::optional<DatasetId> preferred) const {
  std::vector<QString> preferred_keys;
  std::vector<QString> fallback_keys;
  std::optional<DatasetId> fallback_dataset;
  for (const CatalogItem& item : catalog_.items()) {
    if (item.topic_name != topic || !isScalarField(item)) {
      continue;
    }
    if (preferred.has_value() && item.dataset_id == *preferred) {
      preferred_keys.push_back(item.key);
    } else if (!fallback_dataset.has_value() || item.dataset_id == *fallback_dataset) {
      fallback_dataset = item.dataset_id;  // keep all keys from ONE dataset, never a cross-dataset mix
      fallback_keys.push_back(item.key);
    }
  }
  return preferred_keys.empty() ? fallback_keys : preferred_keys;
}

int PendingDisplayBinder::flush(const QSet<QString>& topics) {
  if (entries_.empty()) {
    return 0;
  }

  const bool drain_pass = topics.isEmpty();
  QSet<PlotWidget*> touched;
  int bound = 0;

  auto it = entries_.begin();
  while (it != entries_.end()) {
    PendingDisplayEntry& entry = *it;
    if (entry.targetIsNull()) {
      releaseDemandRefs(entry);
      it = entries_.erase(it);
      continue;
    }

    // The catalog may have gained datasets naming this entry's topic(s) since it
    // was staged (advertise burst after a layout restore) — attach the demand
    // references those datasets now warrant, or the topic never subscribes.
    refreshDemandRefs(entry);

    if (!drain_pass && !topics.contains(entry.path.topic) && (!entry.isXY() || !topics.contains(entry.x_path.topic))) {
      ++it;
      continue;
    }

    if (entry.kind == PendingDisplayEntry::Kind::kSceneLayer) {
      if (tryCompleteSceneEntry(entry)) {
        ++bound;
        releaseDemandRefs(entry);
        it = entries_.erase(it);
      } else {
        ++it;
      }
      continue;
    }

    std::optional<QString> key = resolveEntryPath(entry.path, entry.preferred_dataset);
    std::optional<QString> x_key;
    if (entry.isXY()) {
      x_key = resolveEntryPath(entry.x_path, entry.preferred_dataset);
    }

    // Empty-field placeholder drop whose topic just materialized: bind one curve
    // per scalar field (mirrors dropping the real topic node). Handled here
    // because no single descriptorForPath answer exists for "all of them".
    if (!key.has_value() && !entry.isXY() && entry.path.field.isEmpty()) {
      const std::vector<QString> field_keys = scalarKeysForTopic(entry.path.topic, entry.preferred_dataset);
      if (!field_keys.empty()) {
        PlotWidget* plot = entry.plot.data();
        bool any_bound = false;
        for (const QString& field_key : field_keys) {
          any_bound = plot->addCurve(field_key) != nullptr || any_bound;
        }
        if (any_bound) {
          touched.insert(plot);
          ++bound;
          releaseDemandRefs(entry);
          it = entries_.erase(it);
          continue;
        }
      }
      ++it;
      continue;
    }

    if (!key.has_value() || (entry.isXY() && !x_key.has_value())) {
      ++it;
      continue;
    }

    if (entry.isXY()) {
      entry.curve_element.setAttribute(QStringLiteral("curve_x"), *x_key);
      entry.curve_element.setAttribute(QStringLiteral("curve_y"), *key);
    } else {
      entry.curve_element.setAttribute(QStringLiteral("name"), *key);
    }

    PlotWidget* plot = entry.plot.data();
    if (plot->applyCurveElement(entry.curve_element) == nullptr) {
      ++it;
      continue;
    }

    touched.insert(plot);
    ++bound;
    releaseDemandRefs(entry);
    it = entries_.erase(it);
  }

  for (PlotWidget* plot : touched) {
    // Frame to the layout-saved window now that the dataset (hence its display offset)
    // exists: at restore xmlLoadState ran against an empty catalog (offset 0), so its
    // absolute->display conversion was wrong. applySavedViewportOrZoom re-applies the
    // saved range with the live offset — pinning the final window up front so data
    // fills into it like streaming — and falls back to zoomOut when there is no saved
    // range. clear_after=false: keep the stash; the drain pass clears it.
    plot->applySavedViewportOrZoom(/*clear_after=*/false);
  }
  return bound;
}

QList<layout_xml::SeriesPath> PendingDisplayBinder::unresolved() const {
  QList<layout_xml::SeriesPath> paths;
  for (const PendingDisplayEntry& entry : entries_) {
    if (entry.kind != PendingDisplayEntry::Kind::kCurve || entry.plot.isNull()) {
      continue;
    }
    // Report only the half/halves that still don't resolve: an XY entry can be
    // pending because just one source is missing, and listing a present series as
    // "missing" would mislead the prompt (matches rebindCurveKeys' reporting).
    if (entry.isXY() && !resolveSeriesPath(catalog_, entry.x_path).has_value()) {
      paths.push_back(entry.x_path);
    }
    if (!resolveSeriesPath(catalog_, entry.path).has_value()) {
      paths.push_back(entry.path);
    }
  }
  return paths;
}

void PendingDisplayBinder::clear() {
  for (const PendingDisplayEntry& entry : entries_) {
    releaseDemandRefs(entry);
  }
  entries_.clear();
}

bool PendingDisplayBinder::empty() const {
  return entries_.empty();
}

int PendingDisplayBinder::size() const {
  return static_cast<int>(entries_.size());
}

}  // namespace PJ
