// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutXml.h"

#include <QDomNodeList>
#include <QFileInfo>
#include <QSet>
#include <utility>
#include <vector>
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
    const qsizetype hit = json.indexOf(u"]]>"_s, start);
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
  const QDomElement wrapper = doc.documentElement().firstChildElement(u"previouslyLoaded_Datafiles"_s);
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

    // Source Timeline state (optional; absent in pre-v3 layouts). A missing
    // display_offset_ns leaves has_display_offset false so the reloaded dataset
    // keeps its natural zero offset rather than being explicitly rewritten.
    if (file_info.hasAttribute(u"display_offset_ns"_s)) {
      info.display_offset_ns = file_info.attribute(u"display_offset_ns"_s).toLongLong();
      info.has_display_offset = true;
    }
    info.timeline_order = file_info.attribute(u"timeline_order"_s, u"-1"_s).toInt();

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
  return topic.isEmpty() ? field : topic + QLatin1Char('/') + field;
}

namespace {

// Reads a (topic, field) attribute pair off a curve element into a SeriesPath.
// Returns nullopt when the topic attribute is absent (e.g. a curve that carries
// no stable identity), so callers can skip it cleanly.
std::optional<SeriesPath> readPath(const QDomElement& curve, const QString& topic_attr, const QString& field_attr) {
  if (!curve.hasAttribute(topic_attr)) {
    return std::nullopt;
  }
  return SeriesPath{curve.attribute(topic_attr), curve.attribute(field_attr)};
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

QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc) {
  QList<SeriesPath> paths;
  QSet<QString> seen;
  const auto push = [&](const std::optional<SeriesPath>& p) {
    if (!p.has_value()) {
      return;
    }
    const QString dedup_key = p->topic + QLatin1Char('\x1f') + p->field;
    if (!seen.contains(dedup_key)) {
      seen.insert(dedup_key);
      paths.push_back(*p);
    }
  };
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    push(readPath(curve, u"topic"_s, u"field"_s));
    push(readPath(curve, u"x_topic"_s, u"x_field"_s));
    push(readPath(curve, u"y_topic"_s, u"y_field"_s));
  });
  return paths;
}

QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve) {
  QList<SeriesPath> unresolved;
  QSet<QString> unresolved_seen;
  const auto record_unresolved = [&](const SeriesPath& p) {
    const QString dedup_key = p.topic + QLatin1Char('\x1f') + p.field;
    if (!unresolved_seen.contains(dedup_key)) {
      unresolved_seen.insert(dedup_key);
      unresolved.push_back(p);
    }
  };

  std::vector<QDomElement> curves;
  forEachPlotCurve(doc, [&](const QDomElement& curve) { curves.push_back(curve); });

  for (QDomElement& curve : curves) {
    const std::optional<SeriesPath> xy_x = readPath(curve, u"x_topic"_s, u"x_field"_s);
    if (xy_x.has_value()) {
      // XY curve: both axes must resolve, else the curve is undrawable.
      const std::optional<SeriesPath> xy_y = readPath(curve, u"y_topic"_s, u"y_field"_s);
      const std::optional<QString> x_key = resolve(*xy_x);
      const std::optional<QString> y_key = xy_y.has_value() ? resolve(*xy_y) : std::nullopt;
      if (x_key.has_value() && y_key.has_value()) {
        curve.setAttribute(u"curve_x"_s, *x_key);
        curve.setAttribute(u"curve_y"_s, *y_key);
      } else {
        curve.removeAttribute(u"curve_x"_s);
        curve.removeAttribute(u"curve_y"_s);
        if (!x_key.has_value()) {
          record_unresolved(*xy_x);
        }
        if (xy_y.has_value() && !y_key.has_value()) {
          record_unresolved(*xy_y);
        }
      }
      continue;
    }

    const std::optional<SeriesPath> ts = readPath(curve, u"topic"_s, u"field"_s);
    if (!ts.has_value()) {
      continue;  // No stable identity to rebind; leave as-is.
    }
    if (const std::optional<QString> key = resolve(*ts); key.has_value()) {
      curve.setAttribute(u"name"_s, *key);
    } else {
      curve.removeAttribute(u"name"_s);
      record_unresolved(*ts);
    }
  }
  return unresolved;
}

void stripUnresolvedCurves(QDomDocument& doc) {
  std::vector<QDomNode> victims;
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
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
    state.snap = element.attribute(u"snap"_s) == u"true"_s;
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
