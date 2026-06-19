// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <optional>
#include <vector>

#include "LayoutXml.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"

namespace PJ {

struct PendingCurveEntry {
  QPointer<PlotWidget> plot;      ///< Null means the plot was destroyed while loading.
  layout_xml::SeriesPath path;    ///< Time-series source; for XY this is the Y source.
  layout_xml::SeriesPath x_path;  ///< Non-empty topic means this entry is XY.
  QDomDocument curve_doc;         ///< Owns the detached <curve> clone.
  QDomElement curve_element;      ///< Element imported into curve_doc.

  [[nodiscard]] bool isXY() const {
    return !x_path.topic.isEmpty();
  }
};

// Resolves a stable layout path to the first matching concrete catalog key in dataset load order.
[[nodiscard]] std::optional<QString> resolveSeriesPath(const CatalogModel& catalog, const layout_xml::SeriesPath& path);

// GUI-thread-only registry for curves whose stable topic/field paths were not in the catalog
// when a progressive layout restore built the plots. It owns no widgets; QPointer drops
// entries safely if a plot is destroyed before its source arrives.
class PendingCurveBinder {
 public:
  explicit PendingCurveBinder(CatalogModel& catalog);

  // Re-walks the saved layout and stores unresolved per-plot curves as detached DOM clones.
  void collect(const QDomDocument& doc, const QHash<QString, PlotWidget*>& plots_by_state_id);

  // Attempts pending entries whose topics arrived; an empty set is the drain pass and tries all entries.
  [[nodiscard]] int flush(const QSet<QString>& topics);

  // Remaining unresolved stable paths for a final missing-curve prompt; dead plots are omitted.
  [[nodiscard]] QList<layout_xml::SeriesPath> unresolved() const;

  void clear();
  [[nodiscard]] bool empty() const;
  [[nodiscard]] int size() const;

 private:
  CatalogModel& catalog_;
  std::vector<PendingCurveEntry> entries_;
};

}  // namespace PJ
