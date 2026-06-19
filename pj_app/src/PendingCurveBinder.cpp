// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PendingCurveBinder.h"

#include <QDomNodeList>
#include <utility>

namespace PJ {

std::optional<QString> resolveSeriesPath(const CatalogModel& catalog, const layout_xml::SeriesPath& path) {
  for (const auto& [id, name] : catalog.datasets()) {
    (void)name;
    if (const auto descriptor = catalog.descriptorForPath(id, path.topic, path.field)) {
      return descriptor->name;
    }
  }
  return std::nullopt;
}

PendingCurveBinder::PendingCurveBinder(CatalogModel& catalog) : catalog_(catalog) {}

void PendingCurveBinder::collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id) {
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
      PendingCurveEntry entry;
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
      entries_.push_back(std::move(entry));
    }
  }
}

int PendingCurveBinder::flush(const QSet<QString>& topics) {
  if (entries_.empty()) {
    return 0;
  }

  const bool drain_pass = topics.isEmpty();
  QSet<PlotWidget*> touched;
  int bound = 0;

  auto it = entries_.begin();
  while (it != entries_.end()) {
    PendingCurveEntry& entry = *it;
    if (entry.plot.isNull()) {
      it = entries_.erase(it);
      continue;
    }

    if (!drain_pass && !topics.contains(entry.path.topic) && (!entry.isXY() || !topics.contains(entry.x_path.topic))) {
      ++it;
      continue;
    }

    const std::optional<QString> key = resolveSeriesPath(catalog_, entry.path);
    std::optional<QString> x_key;
    if (entry.isXY()) {
      x_key = resolveSeriesPath(catalog_, entry.x_path);
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

QList<layout_xml::SeriesPath> PendingCurveBinder::unresolved() const {
  QList<layout_xml::SeriesPath> paths;
  for (const PendingCurveEntry& entry : entries_) {
    if (entry.plot.isNull()) {
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

void PendingCurveBinder::clear() {
  entries_.clear();
}

bool PendingCurveBinder::empty() const {
  return entries_.empty();
}

int PendingCurveBinder::size() const {
  return static_cast<int>(entries_.size());
}

}  // namespace PJ
