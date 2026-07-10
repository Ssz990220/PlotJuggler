#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

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

// Per-dataset Source Timeline state nested under one replayable file. One file
// may fan out into several DatasetIds; source_index is the stable position in
// that file's fan-out order (so two fan-out members with identical display
// names can't swap offsets) and source_name disambiguates ordinary unique names.
struct DataSourceDatasetRef {
  QString source_name;
  int source_index = -1;
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // Schema v3 wrote SessionManager::displayOffset() (per-source alignment +
  // global reference); v4 writes sourceDisplayOffset() only. Set true for a v3
  // read so the apply path subtracts the current global reference — see
  // DataSourceRef::display_offset_includes_global_reference.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
};

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
  // These flat fields mirror the first <dataset> child (legacy single-track view
  // for older readers); the per-dataset fan-out state lives in `datasets`.
  qint64 display_offset_ns = 0;
  bool has_display_offset = false;
  // Schema v3 wrote SessionManager::displayOffset(), which included the global
  // relative-time reference. Schema v4 writes sourceDisplayOffset() only. The
  // apply path subtracts its current global reference for this read-only
  // migration, keeping total placement equal to the v3 value without
  // double-applying it. Set true only for a v3 (or older) read.
  bool display_offset_includes_global_reference = false;
  int timeline_order = -1;
  // Schema-v4 additive extension: one entry per dataset the file fanned out
  // into. Empty means an older (≤v3) layout whose single legacy timeline state
  // lives in the flat fields above.
  QList<DataSourceDatasetRef> datasets;
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

// Stable identity of a series: topic + field path, optionally qualified by an
// exact live DatasetId, a portable raw source label, and a full file path. The
// qualifiers stop same-shaped datasets from being interchanged during undo,
// while unqualified legacy layouts still rebind when the structural path is
// globally unique. This replaces the engine's opaque per-load catalog key
// (CurveDescriptor::name); topic and field stay separate because PJ4 field
// paths can themselves contain '/'. The explicit constructor keeps existing
// two-argument brace-initializations compiling.
struct SeriesPath {
  SeriesPath(
      QString topic_in = {}, QString field_in = {}, std::uint32_t dataset_id_in = 0, QString dataset_source_in = {},
      QString dataset_path_in = {})
      : topic(std::move(topic_in)),
        field(std::move(field_in)),
        dataset_id(dataset_id_in),
        dataset_source(std::move(dataset_source_in)),
        dataset_path(std::move(dataset_path_in)) {}

  QString topic;
  QString field;
  // Exact in-session hint for undo/redo (0 = unqualified). A resolver must
  // reject ambiguity rather than silently pick the first dataset exposing the
  // same topic/field.
  std::uint32_t dataset_id = 0;
  // Portable raw source label (DatasetInfo::source_name), usable for layout
  // reload after ids are reminted.
  QString dataset_source;
  // Full file identity stamped by FileLoader at layout-file save. Unlike
  // dataset_source (often a basename/display label), it distinguishes same-named
  // files in different directories and guards a coincidentally reminted id.
  QString dataset_path;

  [[nodiscard]] bool operator==(const SeriesPath& other) const {
    return topic == other.topic && field == other.field && dataset_id == other.dataset_id &&
           dataset_source == other.dataset_source && dataset_path == other.dataset_path;
  }
  // Human-readable form for missing-curve lists: "topic/field".
  [[nodiscard]] QString display() const;
};

// Read one curve's SeriesPath off a <curve> element, honoring the range-checked
// dataset-id parse (an out-of-range or non-numeric id decays to 0/unqualified).
// These are the single reader for curve attributes so hand-built SeriesPath call
// sites (e.g. PendingDisplayBinder) cannot drift from the range-check.
//   * readTimeSeriesPath: the plain topic/field + dataset_* qualifiers.
//   * readXyXPath / readXyYPath: the x_/y_-prefixed XY-axis qualifiers.
// nullopt when the element lacks the relevant topic attribute (no stable identity).
[[nodiscard]] std::optional<SeriesPath> readTimeSeriesPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyXPath(const QDomElement& curve);
[[nodiscard]] std::optional<SeriesPath> readXyYPath(const QDomElement& curve);

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

// Converts a saved workspace document into a portable generic layout by removing
// the exact dataset hints (dataset_id / dataset_source / dataset_path and their
// x_/y_ XY variants) from plotted TS/XY curves, plus input_dataset_* from
// data-processor <processor> inputs. The topic+field identity remains and may bind
// at apply time only when unique in the loaded data. Scene docks are deliberately
// NOT touched — they own their qualifier round-trip and require the numeric id even
// in a generic layout. Undo snapshots and source-bound layouts keep their qualifiers.
void removeDatasetQualifiersForGenericLayout(QDomDocument& doc);

// Maps a serialized DatasetId to its FileLoader-known full source path (empty for
// a non-file/unknown dataset). Supplied to stampDatasetSourcePaths so the XML
// layer needs no FileLoader dependency of its own.
using DatasetPathLookup = std::function<QString(std::uint32_t)>;

// Adds the FileLoader-known full-path companion (`*_dataset_path`) next to every
// persisted `*_dataset_id` on plotted TS/XY curves and data-processor <processor>
// inputs. Scene docks are NOT visited — they own their qualifier round-trip through
// their own save/restore (see removeDatasetQualifiersForGenericLayout). A dataset
// the lookup can't place keeps its id/source qualifier unchanged.
void stampDatasetSourcePaths(QDomDocument& doc, const DatasetPathLookup& lookup);

// Makes a document safe to persist outside the live session. A non-zero numeric
// id WITHOUT a full-path qualifier is only a volatile in-session hint; strip it
// so a later session cannot accept a coincidentally reminted id merely because a
// basename-like source label also matches. Path-qualified ids and zero-valued
// local-scene ids are left intact; the retained source label may still rebind
// when unique. Scoped to plot curves and <processor> inputs (same reason as
// stampDatasetSourcePaths); scene ids survive so restored scene layers keep binding.
void removeUnvalidatedDatasetIds(QDomDocument& doc);

// Resolves relative `*_dataset_path` qualifiers against the layout file's
// directory before any plot/processor/scene restore consumes them, so a
// source-bound layout moves together with its data. Undo snapshots carry no such
// portable path attributes and are unchanged.
void resolveDatasetSourcePaths(QDomDocument& doc, const QDir& layout_dir);

// Removes every <curve> left without any usable key after rebindCurveKeys
// (empty name and empty curve_x/curve_y). Two-pass so the live node list
// isn't invalidated mid-iteration.
void stripUnresolvedCurves(QDomDocument& doc);

// Annotates every <plot>'s <range> with an explicit x_basis marker when it lacks
// one, so the widget loader never has to infer the X coordinate meaning from plot
// mode. A read-side migration: it changes NO numeric range values. An XY plot's
// range becomes x_basis="value"; a time-series range becomes x_basis="absolute"
// because PJ4 has always persisted time-axis ranges in absolute seconds (the v3
// writer already did, per PR #248). Idempotent — a range that already carries
// x_basis is left untouched. Call BEFORE any widget restore consumes the document.
void normalizePlotRangeBasis(QDomDocument& doc);

}  // namespace PJ::layout_xml
