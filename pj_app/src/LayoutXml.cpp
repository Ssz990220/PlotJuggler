// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutXml.h"

#include <QDomNodeList>
#include <QFileInfo>
#include <QSet>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "pj_plotting/PlotXml.h"

using namespace Qt::StringLiterals;

namespace PJ::layout_xml {

QString ensureLayoutExtension(const QString& path) {
  if (path.isEmpty() || !QFileInfo(path).suffix().isEmpty()) {
    return path;
  }
  return path + QLatin1String(kLayoutExtension);
}

void appendJsonAsCdata(QDomDocument& doc, QDomElement& parent, const QString& json) {
  // Indices kept as qsizetype to avoid narrowing on -Werror builds; Qt 6's
  // QString APIs return qsizetype throughout.
  qsizetype start = 0;
  while (true) {
    const qsizetype hit = json.indexOf("]]>"_L1, start);
    if (hit < 0) {
      parent.appendChild(doc.createCDATASection(json.mid(start)));
      return;
    }
    // End this section AFTER "]]" so the next section begins with ">".
    parent.appendChild(doc.createCDATASection(json.mid(start, hit + 2 - start)));
    start = hit + 2;
  }
}

QString directCdataText(const QDomElement& element) {
  QString out;
  for (QDomNode n = element.firstChild(); !n.isNull(); n = n.nextSibling()) {
    if (n.isCDATASection() || n.isText()) {
      out += n.nodeValue();  // child *elements* (e.g. <source_fallback>) are skipped
    }
  }
  return out;
}

QList<DataSourceRef> extractDataSource(const QDomDocument& doc, const QDir& layout_dir) {
  QList<DataSourceRef> sources;
  const QDomElement root = doc.documentElement();
  // Schema <4 baked the global relative-time reference into every persisted
  // display offset; v4+ persists the per-source alignment only. A missing/
  // non-numeric version is treated as legacy (0), so an unversioned document
  // migrates like v3.
  const int schema_version = root.attribute(u"pj4_version"_s, u"0"_s).toInt();
  const QDomElement wrapper = root.firstChildElement(u"previouslyLoaded_Datafiles"_s);
  if (wrapper.isNull()) {
    return sources;
  }
  // Walk every <fileInfo> sibling, not just the first: a multi-file session
  // saves one per loaded data file (see MainWindow::appendDataSourceElement).
  for (QDomElement file_info = wrapper.firstChildElement(u"fileInfo"_s); !file_info.isNull();
       file_info = file_info.nextSiblingElement(u"fileInfo"_s)) {
    const QString filename = file_info.attribute(u"filename"_s);
    if (filename.isEmpty()) {
      continue;
    }
    DataSourceRef info;
    const QFileInfo qfi(filename);
    info.resolved_path = qfi.isAbsolute() ? qfi.absoluteFilePath() : layout_dir.absoluteFilePath(filename);
    info.prefix = file_info.attribute(u"prefix"_s);

    // Legacy single-track state on <fileInfo> (optional; absent in pre-v3
    // layouts). A missing display_offset_ns leaves has_display_offset false so
    // the reloaded dataset keeps its natural zero offset rather than being
    // explicitly rewritten. It is consulted only when no <dataset> children are
    // present (a <=v3 layout that could not describe fan-out tracks).
    if (file_info.hasAttribute(u"display_offset_ns"_s)) {
      bool ok = false;
      info.display_offset_ns = file_info.attribute(u"display_offset_ns"_s).toLongLong(&ok);
      info.has_display_offset = ok;
      info.display_offset_includes_global_reference = ok && schema_version < 4;
    }
    bool order_ok = false;
    info.timeline_order = file_info.attribute(u"timeline_order"_s, u"-1"_s).toInt(&order_ok);
    if (!order_ok) {
      info.timeline_order = -1;
    }

    // Schema-v4 per-dataset children: one <dataset> per fan-out member, each
    // keyed by (source_name, source_index) so identical display names still map
    // to distinct tracks. Empty in a <=v3 layout (falls back to the flat state).
    for (QDomElement dataset = file_info.firstChildElement(u"dataset"_s); !dataset.isNull();
         dataset = dataset.nextSiblingElement(u"dataset"_s)) {
      DataSourceDatasetRef dataset_ref;
      dataset_ref.source_name = dataset.attribute(u"source_name"_s);
      bool source_index_ok = false;
      dataset_ref.source_index = dataset.attribute(u"source_index"_s, u"-1"_s).toInt(&source_index_ok);
      if (!source_index_ok) {
        dataset_ref.source_index = -1;
      }
      if (dataset.hasAttribute(u"display_offset_ns"_s)) {
        bool ok = false;
        dataset_ref.display_offset_ns = dataset.attribute(u"display_offset_ns"_s).toLongLong(&ok);
        dataset_ref.has_display_offset = ok;
        dataset_ref.display_offset_includes_global_reference = ok && schema_version < 4;
      }
      bool dataset_order_ok = false;
      dataset_ref.timeline_order = dataset.attribute(u"timeline_order"_s, u"-1"_s).toInt(&dataset_order_ok);
      if (!dataset_order_ok) {
        dataset_ref.timeline_order = -1;
      }
      info.datasets.push_back(std::move(dataset_ref));
    }

    const QDomElement plugin = file_info.firstChildElement(u"plugin"_s);
    if (!plugin.isNull()) {
      info.plugin_id = plugin.attribute(u"ID"_s);
      // QDomElement::text() concatenates all child text/CDATA — exactly
      // the round-trip of doc.createCDATASection above.
      info.plugin_config_json = plugin.text();
    }
    sources.push_back(std::move(info));
  }

  return sources;
}

QString SeriesPath::display() const {
  const QString path = topic.isEmpty() ? field : topic + QLatin1Char('/') + field;
  return dataset_source.isEmpty() ? path : dataset_source + QLatin1Char(':') + path;
}

namespace {

// Reads a (topic, field) attribute pair plus its dataset qualifiers off a curve
// element into a SeriesPath. Returns nullopt when the topic attribute is absent
// (e.g. a curve that carries no stable identity), so callers can skip it cleanly.
// An out-of-range or non-numeric dataset id decays to 0 (unqualified).
std::optional<SeriesPath> readPath(
    const QDomElement& curve, const QString& topic_attr, const QString& field_attr, const QString& dataset_id_attr,
    const QString& dataset_source_attr, const QString& dataset_path_attr) {
  if (!curve.hasAttribute(topic_attr)) {
    return std::nullopt;
  }
  bool id_ok = false;
  const auto raw_id = curve.attribute(dataset_id_attr).toULongLong(&id_ok);
  const std::uint32_t dataset_id =
      id_ok && raw_id <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(raw_id) : 0;
  return SeriesPath{
      curve.attribute(topic_attr), curve.attribute(field_attr), dataset_id, curve.attribute(dataset_source_attr),
      curve.attribute(dataset_path_attr)};
}

// Dedup key that distinguishes two SeriesPaths differing only by dataset
// qualifier — two same-topic datasets must not collapse into one entry.
QString seriesPathDedupKey(const SeriesPath& path) {
  return QString::number(path.dataset_id) + QLatin1Char('\x1f') + path.dataset_source + QLatin1Char('\x1f') +
         path.dataset_path + QLatin1Char('\x1f') + path.topic + QLatin1Char('\x1f') + path.field;
}

// Visits every <curve> that is a direct child of a <plot> element.
template <typename Fn>
void forEachPlotCurve(const QDomDocument& doc, Fn&& fn) {
  const QDomNodeList plot_nodes = doc.elementsByTagName(u"plot"_s);
  for (int i = 0; i < plot_nodes.size(); ++i) {
    const QDomElement plot = plot_nodes.at(i).toElement();
    if (plot.isNull()) {
      continue;
    }
    for (QDomElement curve = plot.firstChildElement(u"curve"_s); !curve.isNull();
         curve = curve.nextSiblingElement(u"curve"_s)) {
      fn(curve);
    }
  }
}

}  // namespace

std::optional<SeriesPath> readTimeSeriesPath(const QDomElement& curve) {
  return readPath(curve, u"topic"_s, u"field"_s, u"dataset_id"_s, u"dataset_source"_s, u"dataset_path"_s);
}

std::optional<SeriesPath> readXyXPath(const QDomElement& curve) {
  return readPath(curve, u"x_topic"_s, u"x_field"_s, u"x_dataset_id"_s, u"x_dataset_source"_s, u"x_dataset_path"_s);
}

std::optional<SeriesPath> readXyYPath(const QDomElement& curve) {
  return readPath(curve, u"y_topic"_s, u"y_field"_s, u"y_dataset_id"_s, u"y_dataset_source"_s, u"y_dataset_path"_s);
}

QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc) {
  QList<SeriesPath> paths;
  QSet<QString> seen;
  const auto push = [&](const std::optional<SeriesPath>& p) {
    if (!p.has_value()) {
      return;
    }
    const QString dedup_key = seriesPathDedupKey(*p);
    if (!seen.contains(dedup_key)) {
      seen.insert(dedup_key);
      paths.push_back(*p);
    }
  };
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    push(readTimeSeriesPath(curve));
    push(readXyXPath(curve));
    push(readXyYPath(curve));
  });
  return paths;
}

QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve) {
  QList<SeriesPath> unresolved;
  QSet<QString> unresolved_seen;
  const auto record_unresolved = [&](const SeriesPath& p) {
    const QString dedup_key = seriesPathDedupKey(p);
    if (!unresolved_seen.contains(dedup_key)) {
      unresolved_seen.insert(dedup_key);
      unresolved.push_back(p);
    }
  };

  std::vector<QDomElement> curves;
  forEachPlotCurve(doc, [&](const QDomElement& curve) { curves.push_back(curve); });

  for (QDomElement& curve : curves) {
    const bool persistent_intent = plot_xml::isPendingIntent(curve);
    const std::optional<SeriesPath> xy_x = readXyXPath(curve);
    if (xy_x.has_value()) {
      // XY curve: both axes must resolve, else the curve is undrawable.
      const std::optional<SeriesPath> xy_y = readXyYPath(curve);
      const std::optional<QString> x_key = resolve(*xy_x);
      const std::optional<QString> y_key = xy_y.has_value() ? resolve(*xy_y) : std::nullopt;
      if (x_key.has_value() && y_key.has_value()) {
        curve.setAttribute(u"curve_x"_s, *x_key);
        curve.setAttribute(u"curve_y"_s, *y_key);
      } else {
        curve.removeAttribute(u"curve_x"_s);
        curve.removeAttribute(u"curve_y"_s);
        if (!persistent_intent && !x_key.has_value()) {
          record_unresolved(*xy_x);
        }
        if (!persistent_intent && xy_y.has_value() && !y_key.has_value()) {
          record_unresolved(*xy_y);
        }
      }
      continue;
    }

    const std::optional<SeriesPath> ts = readTimeSeriesPath(curve);
    if (!ts.has_value()) {
      continue;  // No stable identity to rebind; leave as-is.
    }
    if (const std::optional<QString> key = resolve(*ts); key.has_value()) {
      curve.setAttribute(u"name"_s, *key);
    } else {
      curve.removeAttribute(u"name"_s);
      if (!persistent_intent) {
        record_unresolved(*ts);
      }
    }
  }
  return unresolved;
}

namespace {

// A (id, path) attribute-pair prefix that carries a dataset qualifier. The
// stamp/strip passes below deliberately touch ONLY plot curves and data-processor
// inputs: scene docks (<layer>/<config_topic>/<robot_model>) persist and REQUIRE
// their own numeric dataset_id (scene restore treats a missing id as "handled,
// skip"), so a whole-tree strip would silently delete every restored scene layer.
struct DatasetIdentityAttributes {
  const char* id;
  const char* source;
  const char* path;
};
// The three curve pairs (time-series + both XY axes), applied to <plot><curve>.
constexpr std::array<DatasetIdentityAttributes, 3> kCurveIdentityAttributes{{
    {"dataset_id", "dataset_source", "dataset_path"},
    {"x_dataset_id", "x_dataset_source", "x_dataset_path"},
    {"y_dataset_id", "y_dataset_source", "y_dataset_path"},
}};
// The single processor-input pair, applied to <processor> (its input rebinds on
// reload exactly like a plotted curve, so it carries the same qualifiers).
constexpr DatasetIdentityAttributes kProcessorInputIdentityAttributes{
    "input_dataset_id", "input_dataset_source", "input_dataset_path"};

// Visits every <processor> that is a descendant of <data_processors>.
template <typename Fn>
void forEachProcessor(QDomDocument& doc, Fn&& fn) {
  const QDomNodeList processors = doc.elementsByTagName(u"processor"_s);
  for (int index = 0; index < processors.size(); ++index) {
    QDomElement processor = processors.at(index).toElement();
    if (!processor.isNull()) {
      fn(processor);
    }
  }
}

// Applies `visit` to the document element and every descendant, iteratively so a
// deep layout never overflows the stack. Only resolveDatasetSourcePaths uses this:
// it merely canonicalizes an existing *_dataset_path attribute (never strips or
// adds an id), so walking the whole tree is safe for any widget family.
template <typename Fn>
void forEachElement(QDomDocument& doc, Fn&& visit) {
  std::vector<QDomElement> pending;
  pending.push_back(doc.documentElement());
  while (!pending.empty()) {
    QDomElement element = pending.back();
    pending.pop_back();
    visit(element);
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
      pending.push_back(child);
    }
  }
}

}  // namespace

void removeDatasetQualifiersForGenericLayout(QDomDocument& doc) {
  // Only plot curves and data-processor inputs are stripped here. Scene docks
  // (<layer>/<config_topic>/<robot_model>) own their qualifier round-trip through
  // their own save/restore and require the numeric id even in a generic layout, so
  // they are deliberately left untouched (a whole-tree strip drops restored layers).
  const auto strip_pair = [](QDomElement& element, const DatasetIdentityAttributes& pair) {
    element.removeAttribute(QString::fromLatin1(pair.id));
    element.removeAttribute(QString::fromLatin1(pair.source));
    element.removeAttribute(QString::fromLatin1(pair.path));
  };
  forEachPlotCurve(doc, [&strip_pair](QDomElement curve) {
    for (const DatasetIdentityAttributes& pair : kCurveIdentityAttributes) {
      strip_pair(curve, pair);
    }
  });
  forEachProcessor(
      doc, [&strip_pair](QDomElement& processor) { strip_pair(processor, kProcessorInputIdentityAttributes); });
}

namespace {

// Stamps the FileLoader-known full path next to `pair`'s numeric id on `element`.
void stampIdentityPair(QDomElement& element, const DatasetIdentityAttributes& pair, const DatasetPathLookup& lookup) {
  const QString id_name = QString::fromLatin1(pair.id);
  if (!element.hasAttribute(id_name)) {
    return;
  }
  bool ok = false;
  const qulonglong raw_id = element.attribute(id_name).toULongLong(&ok);
  if (!ok || raw_id == 0 || raw_id > std::numeric_limits<std::uint32_t>::max()) {
    return;
  }
  const QString path = lookup(static_cast<std::uint32_t>(raw_id));
  if (!path.isEmpty()) {
    element.setAttribute(QString::fromLatin1(pair.path), path);
  }
}

// Strips `pair`'s numeric id from `element` when it carries no full-path
// qualifier (an unvalidated volatile hint that must not survive a file save).
void stripUnvalidatedIdentityPair(QDomElement& element, const DatasetIdentityAttributes& pair) {
  const QString id_name = QString::fromLatin1(pair.id);
  if (!element.hasAttribute(id_name) || !element.attribute(QString::fromLatin1(pair.path)).isEmpty()) {
    return;  // absent, or path-qualified (and thus validatable) — keep it.
  }
  bool ok = false;
  const qulonglong id = element.attribute(id_name).toULongLong(&ok);
  if (ok && id != 0) {  // 0 is a local-scene sentinel, not a remintable id.
    element.removeAttribute(id_name);
  }
}

}  // namespace

void stampDatasetSourcePaths(QDomDocument& doc, const DatasetPathLookup& lookup) {
  if (!lookup) {
    return;
  }
  forEachPlotCurve(doc, [&lookup](QDomElement curve) {
    for (const DatasetIdentityAttributes& pair : kCurveIdentityAttributes) {
      stampIdentityPair(curve, pair, lookup);
    }
  });
  forEachProcessor(doc, [&lookup](QDomElement& processor) {
    stampIdentityPair(processor, kProcessorInputIdentityAttributes, lookup);
  });
}

void removeUnvalidatedDatasetIds(QDomDocument& doc) {
  forEachPlotCurve(doc, [](QDomElement curve) {
    for (const DatasetIdentityAttributes& pair : kCurveIdentityAttributes) {
      stripUnvalidatedIdentityPair(curve, pair);
    }
  });
  forEachProcessor(
      doc, [](QDomElement& processor) { stripUnvalidatedIdentityPair(processor, kProcessorInputIdentityAttributes); });
}

void resolveDatasetSourcePaths(QDomDocument& doc, const QDir& layout_dir) {
  static constexpr std::array<const char*, 5> kPathAttributes{
      "dataset_path", "x_dataset_path", "y_dataset_path", "input_dataset_path", "source_dataset_path"};
  forEachElement(doc, [&layout_dir](QDomElement& element) {
    for (const char* raw_name : kPathAttributes) {
      const QString name = QString::fromLatin1(raw_name);
      if (!element.hasAttribute(name)) {
        continue;
      }
      const QString saved_path = element.attribute(name);
      if (saved_path.isEmpty()) {
        continue;
      }
      const QFileInfo info(saved_path);
      element.setAttribute(
          name, QDir::cleanPath(info.isAbsolute() ? info.absoluteFilePath() : layout_dir.absoluteFilePath(saved_path)));
    }
  });
}

void stripUnresolvedCurves(QDomDocument& doc) {
  std::vector<QDomNode> victims;
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    if (plot_xml::isPendingIntent(curve)) {
      return;
    }
    const bool has_ts_key = !curve.attribute(u"name"_s).isEmpty();
    const bool has_xy_keys = !curve.attribute(u"curve_x"_s).isEmpty() && !curve.attribute(u"curve_y"_s).isEmpty();
    if (!has_ts_key && !has_xy_keys) {
      victims.push_back(curve);
    }
  });
  for (QDomNode& v : victims) {
    v.parentNode().removeChild(v);
  }
}

void normalizePlotRangeBasis(QDomDocument& doc) {
  const QDomNodeList plots = doc.elementsByTagName(u"plot"_s);
  for (int index = 0; index < plots.size(); ++index) {
    const QDomElement plot = plots.at(index).toElement();
    if (plot.isNull()) {
      continue;
    }
    QDomElement range = plot.firstChildElement(u"range"_s);
    // A range already carrying x_basis was written by a current build; leave it
    // (keeps this migration idempotent). Only annotate the historical unmarked form.
    if (range.isNull() || range.hasAttribute(plot_xml::kXBasisAttribute)) {
      continue;
    }
    // No numeric values change. XY plots stored a raw data value; time-series plots
    // have always stored ABSOLUTE seconds (the v3 writer added the display offset),
    // so the pre-marker form maps directly onto today's two bases.
    const bool is_xy = plot.attribute(u"mode"_s) == u"XYPlot"_s;
    range.setAttribute(plot_xml::kXBasisAttribute, is_xy ? plot_xml::kXBasisValue : plot_xml::kXBasisAbsolute);
  }
}

QDomElement writeSourceTimelineViewState(QDomDocument& doc, const SourceTimelineViewState& state) {
  QDomElement element = doc.createElement(u"source_timeline"_s);
  if (state.zoom) {
    // High-precision 'g' so the tiny pixels-per-ns zoom (~1e-7) round-trips exactly.
    element.setAttribute(u"zoom"_s, QString::number(*state.zoom, 'g', 17));
  }
  if (state.scroll_left_ns) {
    element.setAttribute(u"scroll_left_ns"_s, QString::number(*state.scroll_left_ns));
  }
  if (state.name_column_width) {
    element.setAttribute(u"name_column_width"_s, QString::number(*state.name_column_width));
  }
  if (state.snap) {
    element.setAttribute(u"snap"_s, *state.snap ? u"true"_s : u"false"_s);
  }
  return element;
}

SourceTimelineViewState readSourceTimelineViewState(const QDomElement& element) {
  SourceTimelineViewState state;
  if (element.isNull()) {
    return state;
  }
  bool ok = false;
  if (element.hasAttribute(u"zoom"_s)) {
    const double zoom = element.attribute(u"zoom"_s).toDouble(&ok);
    if (ok && zoom > 0.0) {
      state.zoom = zoom;
    }
  }
  if (element.hasAttribute(u"scroll_left_ns"_s)) {
    const qint64 left_ns = element.attribute(u"scroll_left_ns"_s).toLongLong(&ok);
    if (ok) {
      state.scroll_left_ns = left_ns;
    }
  }
  if (element.hasAttribute(u"name_column_width"_s)) {
    const int width = element.attribute(u"name_column_width"_s).toInt(&ok);
    if (ok && width > 0) {
      state.name_column_width = width;
    }
  }
  if (element.hasAttribute(u"snap"_s)) {
    state.snap = element.attribute(u"snap"_s) == "true"_L1;
  }
  return state;
}

bool isSamePath(const QString& a, const QString& b) {
  if (a.isEmpty() || b.isEmpty()) {
    return false;
  }
  const QString canon_a = QFileInfo(a).canonicalFilePath();
  const QString canon_b = QFileInfo(b).canonicalFilePath();
  // QFileInfo::canonicalFilePath() returns empty for non-existent paths.
  // Two missing files shouldn't be treated as "the same" — fall back to
  // a literal comparison only when both inputs resolved to the same
  // non-empty canonical form.
  if (canon_a.isEmpty() || canon_b.isEmpty()) {
    return false;
  }
  return canon_a == canon_b;
}

}  // namespace PJ::layout_xml
