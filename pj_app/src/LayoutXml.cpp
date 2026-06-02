// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "LayoutXml.h"

#include <QDomNodeList>
#include <QFileInfo>
#include <QSet>
#include <vector>

namespace PJ::LayoutXml {

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
    const qsizetype hit = json.indexOf(QStringLiteral("]]>"), start);
    if (hit < 0) {
      parent.appendChild(doc.createCDATASection(json.mid(start)));
      return;
    }
    // End this section AFTER "]]" so the next section begins with ">".
    parent.appendChild(doc.createCDATASection(json.mid(start, hit + 2 - start)));
    start = hit + 2;
  }
}

DataSourceRef extractDataSource(const QDomDocument& doc, const QDir& layout_dir) {
  DataSourceRef info;
  const QDomElement wrapper = doc.documentElement().firstChildElement(QStringLiteral("previouslyLoaded_Datafiles"));
  if (wrapper.isNull()) {
    return info;
  }
  const QDomElement file_info = wrapper.firstChildElement(QStringLiteral("fileInfo"));
  if (file_info.isNull()) {
    return info;
  }
  const QString filename = file_info.attribute(QStringLiteral("filename"));
  if (filename.isEmpty()) {
    return info;
  }
  const QFileInfo qfi(filename);
  info.resolved_path = qfi.isAbsolute() ? qfi.absoluteFilePath() : layout_dir.absoluteFilePath(filename);
  info.prefix = file_info.attribute(QStringLiteral("prefix"));

  const QDomElement plugin = file_info.firstChildElement(QStringLiteral("plugin"));
  if (!plugin.isNull()) {
    info.plugin_id = plugin.attribute(QStringLiteral("ID"));
    // QDomElement::text() concatenates all child text/CDATA — exactly
    // the round-trip of doc.createCDATASection above.
    info.plugin_config_json = plugin.text();
  }

  return info;
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
  const QDomNodeList plot_nodes = doc.elementsByTagName(QStringLiteral("plot"));
  for (int i = 0; i < plot_nodes.size(); ++i) {
    const QDomElement plot = plot_nodes.at(i).toElement();
    if (plot.isNull()) {
      continue;
    }
    for (QDomElement curve = plot.firstChildElement(QStringLiteral("curve")); !curve.isNull();
         curve = curve.nextSiblingElement(QStringLiteral("curve"))) {
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
    push(readPath(curve, QStringLiteral("topic"), QStringLiteral("field")));
    push(readPath(curve, QStringLiteral("x_topic"), QStringLiteral("x_field")));
    push(readPath(curve, QStringLiteral("y_topic"), QStringLiteral("y_field")));
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
    const std::optional<SeriesPath> xy_x = readPath(curve, QStringLiteral("x_topic"), QStringLiteral("x_field"));
    if (xy_x.has_value()) {
      // XY curve: both axes must resolve, else the curve is undrawable.
      const std::optional<SeriesPath> xy_y = readPath(curve, QStringLiteral("y_topic"), QStringLiteral("y_field"));
      const std::optional<QString> x_key = resolve(*xy_x);
      const std::optional<QString> y_key = xy_y.has_value() ? resolve(*xy_y) : std::nullopt;
      if (x_key.has_value() && y_key.has_value()) {
        curve.setAttribute(QStringLiteral("curve_x"), *x_key);
        curve.setAttribute(QStringLiteral("curve_y"), *y_key);
      } else {
        curve.removeAttribute(QStringLiteral("curve_x"));
        curve.removeAttribute(QStringLiteral("curve_y"));
        if (!x_key.has_value()) {
          record_unresolved(*xy_x);
        }
        if (xy_y.has_value() && !y_key.has_value()) {
          record_unresolved(*xy_y);
        }
      }
      continue;
    }

    const std::optional<SeriesPath> ts = readPath(curve, QStringLiteral("topic"), QStringLiteral("field"));
    if (!ts.has_value()) {
      continue;  // No stable identity to rebind; leave as-is.
    }
    if (const std::optional<QString> key = resolve(*ts); key.has_value()) {
      curve.setAttribute(QStringLiteral("name"), *key);
    } else {
      curve.removeAttribute(QStringLiteral("name"));
      record_unresolved(*ts);
    }
  }
  return unresolved;
}

void stripUnresolvedCurves(QDomDocument& doc) {
  std::vector<QDomNode> victims;
  forEachPlotCurve(doc, [&](const QDomElement& curve) {
    const bool has_ts_key = !curve.attribute(QStringLiteral("name")).isEmpty();
    const bool has_xy_keys =
        !curve.attribute(QStringLiteral("curve_x")).isEmpty() && !curve.attribute(QStringLiteral("curve_y")).isEmpty();
    if (!has_ts_key && !has_xy_keys) {
      victims.push_back(curve);
    }
  });
  for (QDomNode& v : victims) {
    v.parentNode().removeChild(v);
  }
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

}  // namespace PJ::LayoutXml
