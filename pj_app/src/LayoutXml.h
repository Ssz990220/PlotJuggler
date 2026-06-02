#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace PJ::LayoutXml {

// Canonical layout-file extension, leading dot included. Double extension
// so the files read as XML to editors/tools while staying identifiable as
// PJ4 layouts. Single source of truth for the dialog filter, the dialog's
// default suffix, and ensureLayoutExtension below.
inline constexpr char kLayoutExtension[] = ".pj4.xml";

// Returns `path` with kLayoutExtension appended, but only when it carries
// no extension at all (QFileInfo::suffix() empty). A path the user already
// typed an extension for — including ".pj4.xml" — is returned unchanged.
// Backstops the Save dialog's defaultSuffix so a bare typed name always
// lands as a .pj4.xml file regardless of platform dialog quirks. Empty in,
// empty out.
[[nodiscard]] QString ensureLayoutExtension(const QString& path);

// Resolved data-source reference extracted from <previouslyLoaded_Datafiles>.
// Empty resolved_path means no replayable source was found in the layout.
struct DataSourceRef {
  QString resolved_path;
  QString prefix;
  QString plugin_id;           // Empty when the layout had no <plugin> child.
  QString plugin_config_json;  // Empty when the layout had no <plugin> child.
};

// CDATA sections cannot contain "]]>"; QDomDocument::createCDATASection
// does not escape it. Splits the payload across adjacent CDATA sections
// at each "]]>" boundary so QDomElement::text() concatenates them back
// transparently on read.
void appendJsonAsCdata(QDomDocument& doc, QDomElement& parent, const QString& json);

// Reads <previouslyLoaded_Datafiles>/<fileInfo> and the optional
// <plugin> child. resolved_path is absolute — relatives are anchored
// at `layout_dir`. Returns an empty resolved_path when the element is
// absent, malformed, or carries no filename.
[[nodiscard]] DataSourceRef extractDataSource(const QDomDocument& doc, const QDir& layout_dir);

// True iff both paths resolve to the same on-disk file. Used by the
// layout-load data-source replay to decide whether the currently
// loaded source is the one the layout references (and re-load is a
// no-op) or a different file (and the user should be prompted).
// Compares via QFileInfo::canonicalFilePath, so relative-vs-absolute,
// trailing-slash, and symlink variations all collapse correctly.
// Empty inputs are treated as "not the same".
[[nodiscard]] bool isSamePath(const QString& a, const QString& b);

// Stable, file-portable identity of a series: the topic plus the field path
// within that topic (e.g. "/vehicle/imu" + "linear_accel.x"). Dataset-agnostic,
// so a layout built on one recording rebinds to a similar one with the same
// topics/fields. This replaces the engine's opaque per-load catalog key
// (CurveDescriptor::name) as the persisted identity in v2 layouts, mirroring
// PJ3's human series-name identity (split into two attributes only because PJ4
// field paths can themselves contain '/').
struct SeriesPath {
  QString topic;
  QString field;

  [[nodiscard]] bool operator==(const SeriesPath& other) const {
    return topic == other.topic && field == other.field;
  }
  // Human-readable form for missing-curve lists: "topic/field".
  [[nodiscard]] QString display() const;
};

// Resolves a stable SeriesPath to a concrete catalog key within the target
// dataset, or std::nullopt when that dataset has no matching topic+field.
using SeriesKeyResolver = std::function<std::optional<QString>(const SeriesPath&)>;

// Collects the distinct SeriesPaths referenced by <curve> children of <plot>
// elements: time-series curves carry topic/field attributes; XY curves carry
// x_topic/x_field and y_topic/y_field. Order-preserving, de-duplicated.
[[nodiscard]] QList<SeriesPath> extractSeriesPaths(const QDomDocument& doc);

// Rewrites each <curve>'s concrete key attributes (name for time-series;
// curve_x/curve_y for XY) to the target dataset's keys, resolving each curve's
// stable topic/field via `resolve`. A curve whose path(s) don't resolve has
// those key attributes cleared and its path(s) returned in the (de-duplicated)
// result so the caller can prompt/strip. In place.
[[nodiscard]] QList<SeriesPath> rebindCurveKeys(QDomDocument& doc, const SeriesKeyResolver& resolve);

// Removes every <curve> left without any usable key after rebindCurveKeys
// (empty name and empty curve_x/curve_y). Two-pass so the live node list
// isn't invalidated mid-iteration.
void stripUnresolvedCurves(QDomDocument& doc);

}  // namespace PJ::LayoutXml
