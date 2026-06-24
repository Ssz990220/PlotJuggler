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

namespace PJ::layout_xml {

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
  // Source Timeline state, re-bound by source path on reload (DatasetIds are
  // re-minted each session, so the file path is the only stable identity).
  // display_offset_ns is the per-source display shift (display = raw - offset);
  // has_display_offset is false when the layout predates this attribute, so the
  // reloaded dataset keeps its natural zero offset. timeline_order is the bar's
  // top-to-bottom slot in the timeline (-1 when absent → fall back to load order).
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  int timeline_order = -1;
};

// CDATA sections cannot contain "]]>"; QDomDocument::createCDATASection
// does not escape it. Splits the payload across adjacent CDATA sections
// at each "]]>" boundary so QDomElement::text() concatenates them back
// transparently on read.
void appendJsonAsCdata(QDomDocument& doc, QDomElement& parent, const QString& json);

// Concatenates ONLY the direct CDATA/text child nodes of `element`, skipping any
// nested child *elements*. QDomElement::text() recurses the entire subtree, so an
// element that holds its own CDATA payload alongside a child element with its own
// CDATA (e.g. a <processor> carrying params-CDATA plus a <source_fallback> child
// holding the filter's Luau source) cannot read its own payload via text()
// without the child's leaking in. This reads the element's own payload only.
[[nodiscard]] QString directCdataText(const QDomElement& element);

// Reads every <previouslyLoaded_Datafiles>/<fileInfo> and its optional
// <plugin> child, one DataSourceRef per file in document order. Each
// resolved_path is absolute — relatives are anchored at `layout_dir`.
// fileInfo entries with no filename are skipped. Returns an empty list when
// the wrapper element is absent or holds no usable fileInfo. Multiple entries
// support multi-file sessions; single-file layouts yield a one-element list.
[[nodiscard]] QList<DataSourceRef> extractDataSource(const QDomDocument& doc, const QDir& layout_dir);

// Source Timeline view chrome persisted as the <source_timeline> element: pure
// view state, independent of the per-source offsets/order (those round-trip via
// DataSourceRef). Each field is optional so a layout that omits an attribute — or
// predates it — leaves that aspect of the widget untouched on restore.
struct SourceTimelineViewState {
  std::optional<double> zoom;            // pixels-per-ns (Ctrl+wheel zoom); only > 0 is valid
  std::optional<qint64> scroll_left_ns;  // display-ns at the viewport's left edge
  std::optional<int> name_column_width;  // left name-column width (px); only > 0 is valid
  std::optional<bool> snap;              // edge-snap-while-dragging toggle
};

// Serialize / parse the <source_timeline> element. write builds a fresh element
// on `doc`, emitting only the set fields (zoom at 17 sig-figs so the ~1e-7
// pixels-per-ns round-trips exactly). read pulls the attributes back, leaving a
// field nullopt when its attribute is absent or malformed (zoom/width also
// require a positive value). Pure Qt-DOM, no widget — the host applies the parsed
// state to the widget, keeping the encode/decode unit-testable.
[[nodiscard]] QDomElement writeSourceTimelineViewState(QDomDocument& doc, const SourceTimelineViewState& state);
[[nodiscard]] SourceTimelineViewState readSourceTimelineViewState(const QDomElement& element);

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

}  // namespace PJ::layout_xml
