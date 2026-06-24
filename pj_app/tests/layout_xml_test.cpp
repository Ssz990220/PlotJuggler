// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "LayoutXml.h"

namespace {

using PJ::layout_xml::DataSourceRef;

// ---------- appendJsonAsCdata ----------------------------------------------

QString roundTripJson(const QString& input) {
  QDomDocument doc;
  QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
  PJ::layout_xml::appendJsonAsCdata(doc, plugin, input);
  doc.appendChild(plugin);

  // Round-trip through serialize -> reparse to confirm the document
  // survives the actual XML pipeline (not just text() of the in-memory
  // DOM, which would only test our writer, not the reader).
  const QByteArray serialized = doc.toByteArray(2);
  QDomDocument reparsed;
  if (!reparsed.setContent(serialized)) {
    return QStringLiteral("__PARSE_FAILED__");
  }
  return reparsed.documentElement().text();
}

TEST(AppendJsonAsCdata, RoundTripsSimpleJson) {
  const QString in = QStringLiteral(R"({"topics":["/imu/accel"],"time":"publish"})");
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsEmptyString) {
  EXPECT_EQ(roundTripJson(QString()), QString());
}

TEST(AppendJsonAsCdata, RoundTripsJsonContainingClosingCdata) {
  // The literal "]]>" inside a CDATA section would close it. The helper
  // splits at every "]]>" boundary; text() on read concatenates them
  // back into the original string.
  const QString in = QStringLiteral(R"({"pattern":"end ]]> middle ]]> tail"})");
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsJsonStartingWithClosingCdata) {
  const QString in = QStringLiteral("]]>{\"x\":1}");
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsJsonEndingWithClosingCdata) {
  const QString in = QStringLiteral("{\"x\":1}]]>");
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsConsecutiveClosingCdataSequences) {
  const QString in = QStringLiteral("a]]>]]>b]]>c");
  EXPECT_EQ(roundTripJson(in), in);
}

TEST(AppendJsonAsCdata, RoundTripsUnicodeAndAngleBrackets) {
  // Tests that QDomDocument doesn't choke on <, >, &, unicode within CDATA.
  const QString in = QStringLiteral(R"({"label":"<x & y> ünïcødé"})");
  EXPECT_EQ(roundTripJson(in), in);
}

// ---------- directCdataText (params vs <source_fallback> separation) --------
//
// A <processor> layout element (M7) carries its params as a DIRECT CDATA child
// AND an optional <source_fallback> CHILD ELEMENT holding the filter's Luau
// source. QDomElement::text() recurses the whole subtree, so reading params via
// processor.text() would slurp the embedded source in too. directCdataText reads
// only the element's own direct CDATA, keeping the two payloads independent.

QDomElement buildProcessorWithSource(QDomDocument& doc, const QString& params, const QString& source) {
  QDomElement processor = doc.createElement(QStringLiteral("processor"));
  PJ::layout_xml::appendJsonAsCdata(doc, processor, params);  // params: a direct CDATA child
  QDomElement src = doc.createElement(QStringLiteral("source_fallback"));
  PJ::layout_xml::appendJsonAsCdata(doc, src, source);
  processor.appendChild(src);  // source: nested inside a child element
  doc.appendChild(processor);
  return processor;
}

TEST(DirectCdataText, ReadsOwnPayloadIgnoringChildElement) {
  QDomDocument doc;
  const QString params = QStringLiteral(R"({"value_scale":2.5})");
  const QString source = QStringLiteral("return { id='scale', create=function(p) end }");
  QDomElement processor = buildProcessorWithSource(doc, params, source);

  EXPECT_EQ(PJ::layout_xml::directCdataText(processor), params);
  EXPECT_EQ(processor.firstChildElement(QStringLiteral("source_fallback")).text(), source);
  // Document the gotcha being guarded against: text() recurses and merges both.
  EXPECT_EQ(processor.text(), params + source);
}

TEST(DirectCdataText, SurvivesSerializeReparseWithClosingCdata) {
  QDomDocument doc;
  const QString params = QStringLiteral(R"({"pat":"a ]]> b"})");
  const QString source = QStringLiteral("-- ]]> in source\nreturn {}");
  buildProcessorWithSource(doc, params, source);

  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QDomElement processor = reparsed.documentElement();
  EXPECT_EQ(PJ::layout_xml::directCdataText(processor), params);
  EXPECT_EQ(processor.firstChildElement(QStringLiteral("source_fallback")).text(), source);
}

TEST(DirectCdataText, EmptyWhenNoDirectCdata) {
  QDomDocument doc;
  QDomElement processor = doc.createElement(QStringLiteral("processor"));
  QDomElement src = doc.createElement(QStringLiteral("source_fallback"));
  PJ::layout_xml::appendJsonAsCdata(doc, src, QStringLiteral("source-only"));
  processor.appendChild(src);
  doc.appendChild(processor);
  EXPECT_TRUE(PJ::layout_xml::directCdataText(processor).isEmpty());
}

// ---------- extractDataSource ----------------------------------------------

QDomDocument buildDataSourceDoc(
    const QString& filename, const QString& prefix = QString(), const QString& plugin_id = QString(),
    const QString& plugin_json = QString()) {
  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  doc.appendChild(root);
  QDomElement wrapper = doc.createElement(QStringLiteral("previouslyLoaded_Datafiles"));
  root.appendChild(wrapper);
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  if (!filename.isNull()) {
    file_info.setAttribute(QStringLiteral("filename"), filename);
  }
  file_info.setAttribute(QStringLiteral("prefix"), prefix);
  if (!plugin_id.isEmpty()) {
    QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
    plugin.setAttribute(QStringLiteral("ID"), plugin_id);
    PJ::layout_xml::appendJsonAsCdata(doc, plugin, plugin_json);
    file_info.appendChild(plugin);
  }
  wrapper.appendChild(file_info);
  return doc;
}

// Appends an extra <fileInfo> to an existing data-source doc, so a single doc
// can carry multiple loaded files (mirrors a multi-file session save).
void appendFileInfo(
    QDomDocument& doc, const QString& filename, const QString& prefix = QString(), const QString& plugin_id = QString(),
    const QString& plugin_json = QString()) {
  QDomElement wrapper = doc.documentElement().firstChildElement(QStringLiteral("previouslyLoaded_Datafiles"));
  QDomElement file_info = doc.createElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("filename"), filename);
  file_info.setAttribute(QStringLiteral("prefix"), prefix);
  if (!plugin_id.isEmpty()) {
    QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
    plugin.setAttribute(QStringLiteral("ID"), plugin_id);
    PJ::layout_xml::appendJsonAsCdata(doc, plugin, plugin_json);
    file_info.appendChild(plugin);
  }
  wrapper.appendChild(file_info);
}

TEST(ExtractDataSource, EmptyDocReturnsEmptyList) {
  QDomDocument doc;
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, MissingWrapperReturnsEmptyList) {
  QDomDocument doc;
  doc.appendChild(doc.createElement(QStringLiteral("root")));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, EmptyFilenameAttributeIsSkipped) {
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral(""));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(refs.isEmpty());
}

TEST(ExtractDataSource, AbsolutePathPassesThrough) {
  // Build a genuinely-absolute path for the host platform. A hardcoded POSIX
  // path like "/tmp/x" is drive-relative on Windows, so QFileInfo would anchor
  // it at the current drive and the equality check would fail.
  const QString abs = QDir::tempPath() + QStringLiteral("/some_data.mcap");
  const QDomDocument doc = buildDataSourceDoc(abs);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(QDir::rootPath()));
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().resolved_path, QFileInfo(abs).absoluteFilePath());
}

TEST(ExtractDataSource, RelativePathIsAnchoredAtLayoutDir) {
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral("data/run.csv"));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir(QStringLiteral("/tmp/layouts")));
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().resolved_path, QStringLiteral("/tmp/layouts/data/run.csv"));
}

TEST(ExtractDataSource, PluginIdAndCdataJsonRoundTrip) {
  const QString json = QStringLiteral(R"({"topics":["a","b"]})");
  const QDomDocument doc =
      buildDataSourceDoc(QStringLiteral("/tmp/x.mcap"), QStringLiteral("robot"), QStringLiteral("DataLoad MCAP"), json);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().prefix, QStringLiteral("robot"));
  EXPECT_EQ(refs.front().plugin_id, QStringLiteral("DataLoad MCAP"));
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, PluginCdataWithClosingSequenceRoundTrips) {
  const QString json = QStringLiteral(R"({"pat":"weird ]]> in middle"})");
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/x.mcap"), QString(), QStringLiteral("CSV"), json);
  // Round-trip the WHOLE doc through serialize+reparse to confirm the
  // CDATA splitting survives the actual file pipeline.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_EQ(refs.front().plugin_config_json, json);
}

TEST(ExtractDataSource, MultipleFileInfosParsedInOrder) {
  // A multi-file session: two distinct files, each with its own plugin config.
  // Host-absolute paths (QDir::tempPath()) — a hardcoded POSIX "/tmp/x" is
  // drive-relative on Windows, so extractDataSource would re-anchor it and the
  // equality check would fail.
  const QString abs_a = QDir::tempPath() + QStringLiteral("/a.mcap");
  const QString abs_b = QDir::tempPath() + QStringLiteral("/b.mcap");
  const QString json_a = QStringLiteral(R"({"topics":["/a"]})");
  const QString json_b = QStringLiteral(R"({"topics":["/b"]})");
  QDomDocument doc = buildDataSourceDoc(abs_a, QString(), QStringLiteral("DataLoad MCAP"), json_a);
  appendFileInfo(doc, abs_b, QStringLiteral("robot"), QStringLiteral("DataLoad MCAP"), json_b);

  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 2);
  EXPECT_EQ(refs[0].resolved_path, QFileInfo(abs_a).absoluteFilePath());
  EXPECT_EQ(refs[0].plugin_config_json, json_a);
  EXPECT_EQ(refs[1].resolved_path, QFileInfo(abs_b).absoluteFilePath());
  EXPECT_EQ(refs[1].prefix, QStringLiteral("robot"));
  EXPECT_EQ(refs[1].plugin_config_json, json_b);
}

TEST(ExtractDataSource, MultiFileSurvivesSerializeReparse) {
  // The full file pipeline: build two fileInfos, serialize, reparse, and
  // confirm both come back in order — the round-trip a saved/loaded layout takes.
  // Host-absolute paths so the assertion holds cross-platform (see above).
  const QString abs_a = QDir::tempPath() + QStringLiteral("/a.mcap");
  const QString abs_b = QDir::tempPath() + QStringLiteral("/b.mcap");
  QDomDocument doc = buildDataSourceDoc(abs_a);
  appendFileInfo(doc, abs_b);
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 2);
  EXPECT_EQ(refs[0].resolved_path, QFileInfo(abs_a).absoluteFilePath());
  EXPECT_EQ(refs[1].resolved_path, QFileInfo(abs_b).absoluteFilePath());
}

// ---------- Source Timeline state (v3) --------------------------------------

// Stamps the per-source timeline attributes onto a doc's only <fileInfo>, as
// MainWindow::appendDataSourceElement does at save time.
void setTimelineState(QDomDocument& doc, qint64 offset_ns, int order) {
  QDomElement file_info = doc.documentElement()
                              .firstChildElement(QStringLiteral("previouslyLoaded_Datafiles"))
                              .firstChildElement(QStringLiteral("fileInfo"));
  file_info.setAttribute(QStringLiteral("display_offset_ns"), QString::number(offset_ns));
  file_info.setAttribute(QStringLiteral("timeline_order"), QString::number(order));
}

TEST(ExtractDataSource, TimelineStateAttributesParsed) {
  QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/run.mcap"));
  setTimelineState(doc, /*offset_ns=*/-1'500'000'000LL, /*order=*/2);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, -1'500'000'000LL);
  EXPECT_EQ(refs.front().timeline_order, 2);
}

TEST(ExtractDataSource, TimelineStateAbsentLeavesDefaults) {
  // A pre-v3 layout (no timeline attributes): the reloaded dataset must keep its
  // natural zero offset (has_display_offset=false → caller skips the write) and
  // fall back to load order (timeline_order=-1).
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/run.mcap"));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_FALSE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 0);
  EXPECT_EQ(refs.front().timeline_order, -1);
}

TEST(ExtractDataSource, TimelineStateSurvivesSerializeReparse) {
  // The realistic path: stamp attrs, serialize to bytes, reparse — the exact
  // round-trip a saved/loaded layout file takes.
  QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/run.mcap"));
  setTimelineState(doc, /*offset_ns=*/42'000LL, /*order=*/0);
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(reparsed, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 42'000LL);
  EXPECT_EQ(refs.front().timeline_order, 0);
}

// A zero offset is meaningful (a dataset deliberately at its natural position),
// so it must be written-and-parsed as present, not conflated with "absent".
TEST(ExtractDataSource, TimelineStateZeroOffsetIsStillPresent) {
  QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/run.mcap"));
  setTimelineState(doc, /*offset_ns=*/0, /*order=*/1);
  const QList<DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, QDir::current());
  ASSERT_EQ(refs.size(), 1);
  EXPECT_TRUE(refs.front().has_display_offset);
  EXPECT_EQ(refs.front().display_offset_ns, 0);
  EXPECT_EQ(refs.front().timeline_order, 1);
}

// ---------- isSamePath ------------------------------------------------------
//
// Codifies the data-source-replay bug: loadLayoutFromPath used to skip the
// reload whenever ANY source was loaded, instead of checking whether the
// currently-loaded source matched the one the layout referenced. A layout
// for file A would then no-op when file B was open, leaving the catalog
// populated with B's keys and every A-key reported as "missing".
// isSamePath() is what gates that decision now.

TEST(IsSamePath, EmptyInputsAreNotSame) {
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QStringLiteral("/tmp/x"), QString()));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(QString(), QStringLiteral("/tmp/x")));
}

TEST(IsSamePath, DifferentExistingFilesAreNotSame) {
  // The exact bug scenario: two real files with different basenames.
  // Before the fix, loadLayoutFromPath treated "anything loaded" as
  // "skip" — this test would have passed even when it shouldn't have.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString a = dir.filePath(QStringLiteral("sagod.mcap"));
  const QString b = dir.filePath(QStringLiteral("zeg.mcap"));
  ASSERT_TRUE(QFile(a).open(QIODevice::WriteOnly));
  ASSERT_TRUE(QFile(b).open(QIODevice::WriteOnly));
  EXPECT_FALSE(PJ::layout_xml::isSamePath(a, b));
}

TEST(IsSamePath, IdenticalAbsolutePathsAreSame) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString p = dir.filePath(QStringLiteral("log.mcap"));
  ASSERT_TRUE(QFile(p).open(QIODevice::WriteOnly));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(p, p));
}

TEST(IsSamePath, RelativeAndAbsoluteFormsOfSameFileAreSame) {
  // The data-source XML may store a relative path; the SessionManager
  // may carry the absolute form. Canonicalization has to collapse them.
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString abs = dir.filePath(QStringLiteral("log.mcap"));
  ASSERT_TRUE(QFile(abs).open(QIODevice::WriteOnly));

  const QString cwd_before = QDir::currentPath();
  ASSERT_TRUE(QDir::setCurrent(dir.path()));
  EXPECT_TRUE(PJ::layout_xml::isSamePath(abs, QStringLiteral("log.mcap")));
  EXPECT_TRUE(QDir::setCurrent(cwd_before));
}

TEST(IsSamePath, NonexistentPathsAreNotSame) {
  // canonicalFilePath() returns empty for missing files; treating those
  // as "same" would mean two layouts that reference deleted files would
  // skip the reload prompt — the opposite of helpful.
  EXPECT_FALSE(
      PJ::layout_xml::isSamePath(QStringLiteral("/nonexistent/a.mcap"), QStringLiteral("/nonexistent/b.mcap")));
  EXPECT_FALSE(
      PJ::layout_xml::isSamePath(QStringLiteral("/nonexistent/a.mcap"), QStringLiteral("/nonexistent/a.mcap")));
}

TEST(EnsureLayoutExtension, AppendsWhenNoExtension) {
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(QStringLiteral("my_layout")), QStringLiteral("my_layout.pj4.xml"));
  EXPECT_EQ(
      PJ::layout_xml::ensureLayoutExtension(QStringLiteral("/home/user/setup")),
      QStringLiteral("/home/user/setup.pj4.xml"));
}

TEST(EnsureLayoutExtension, LeavesCorrectExtensionUntouched) {
  EXPECT_EQ(
      PJ::layout_xml::ensureLayoutExtension(QStringLiteral("my_layout.pj4.xml")), QStringLiteral("my_layout.pj4.xml"));
}

TEST(EnsureLayoutExtension, RespectsAnyUserSpecifiedExtension) {
  // Contract is "append only when no extension is specified", so a name the
  // user deliberately gave another suffix is left alone rather than turned
  // into a double extension like notes.txt.pj4.xml.
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(QStringLiteral("notes.txt")), QStringLiteral("notes.txt"));
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(QStringLiteral("my.layout")), QStringLiteral("my.layout"));
}

TEST(EnsureLayoutExtension, EmptyInEmptyOut) {
  EXPECT_EQ(PJ::layout_xml::ensureLayoutExtension(QString()), QString());
}

// ---------- SeriesPath / extractSeriesPaths / rebindCurveKeys ---------------

using PJ::layout_xml::SeriesPath;

// A doc with one <root><plot>; callers append <curve> elements to the plot.
struct PlotDoc {
  QDomDocument doc;
  QDomElement plot;
};
PlotDoc makePlotDoc() {
  PlotDoc pd;
  QDomElement root = pd.doc.createElement(QStringLiteral("root"));
  pd.doc.appendChild(root);
  pd.plot = pd.doc.createElement(QStringLiteral("plot"));
  root.appendChild(pd.plot);
  return pd;
}
QDomElement addTsCurve(PlotDoc& pd, const QString& topic, const QString& field) {
  QDomElement c = pd.doc.createElement(QStringLiteral("curve"));
  c.setAttribute(QStringLiteral("topic"), topic);
  c.setAttribute(QStringLiteral("field"), field);
  pd.plot.appendChild(c);
  return c;
}
QDomElement addXyCurve(PlotDoc& pd, const SeriesPath& x, const SeriesPath& y) {
  QDomElement c = pd.doc.createElement(QStringLiteral("curve"));
  c.setAttribute(QStringLiteral("x_topic"), x.topic);
  c.setAttribute(QStringLiteral("x_field"), x.field);
  c.setAttribute(QStringLiteral("y_topic"), y.topic);
  c.setAttribute(QStringLiteral("y_field"), y.field);
  pd.plot.appendChild(c);
  return c;
}

TEST(SeriesPathDisplay, JoinsTopicAndField) {
  EXPECT_EQ((SeriesPath{QStringLiteral("/imu"), QStringLiteral("accel.x")}).display(), QStringLiteral("/imu/accel.x"));
  EXPECT_EQ((SeriesPath{QString(), QStringLiteral("lonely")}).display(), QStringLiteral("lonely"));
}

TEST(ExtractSeriesPaths, CollectsTimeSeriesTopicField) {
  PlotDoc pd = makePlotDoc();
  addTsCurve(pd, QStringLiteral("/imu"), QStringLiteral("accel.x"));
  addTsCurve(pd, QStringLiteral("/imu"), QStringLiteral("accel.y"));
  const QList<SeriesPath> paths = PJ::layout_xml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], (SeriesPath{QStringLiteral("/imu"), QStringLiteral("accel.x")}));
  EXPECT_EQ(paths[1], (SeriesPath{QStringLiteral("/imu"), QStringLiteral("accel.y")}));
}

TEST(ExtractSeriesPaths, CollectsXyAxesAndDeduplicates) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{QStringLiteral("/t"), QStringLiteral("a")};
  const SeriesPath y{QStringLiteral("/t"), QStringLiteral("b")};
  addXyCurve(pd, x, y);
  addTsCurve(pd, QStringLiteral("/t"), QStringLiteral("a"));  // duplicate of x
  const QList<SeriesPath> paths = PJ::layout_xml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], x);
  EXPECT_EQ(paths[1], y);
}

TEST(ExtractSeriesPaths, SkipsCurvesWithoutStableIdentity) {
  PlotDoc pd = makePlotDoc();
  QDomElement legacy = pd.doc.createElement(QStringLiteral("curve"));
  legacy.setAttribute(QStringLiteral("name"), QStringLiteral("dataset:1/topic:2/column:0"));
  pd.plot.appendChild(legacy);
  EXPECT_TRUE(PJ::layout_xml::extractSeriesPaths(pd.doc).isEmpty());
}

TEST(RebindCurveKeys, SetsNameForResolvedTimeSeries) {
  PlotDoc pd = makePlotDoc();
  QDomElement c = addTsCurve(pd, QStringLiteral("/imu"), QStringLiteral("accel.x"));
  const auto resolve = [](const SeriesPath& p) -> std::optional<QString> {
    if (p.topic == QStringLiteral("/imu") && p.field == QStringLiteral("accel.x")) {
      return QStringLiteral("dataset:7/topic:3/column:0");
    }
    return std::nullopt;
  };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_TRUE(unresolved.isEmpty());
  EXPECT_EQ(c.attribute(QStringLiteral("name")), QStringLiteral("dataset:7/topic:3/column:0"));
}

TEST(RebindCurveKeys, ClearsNameAndReportsUnresolvedTimeSeries) {
  PlotDoc pd = makePlotDoc();
  QDomElement c = addTsCurve(pd, QStringLiteral("/missing"), QStringLiteral("f"));
  c.setAttribute(QStringLiteral("name"), QStringLiteral("stale_key"));  // stale from prior session
  const auto resolve = [](const SeriesPath&) -> std::optional<QString> { return std::nullopt; };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{QStringLiteral("/missing"), QStringLiteral("f")}));
  EXPECT_FALSE(c.hasAttribute(QStringLiteral("name")));  // stale key cleared, won't mis-resolve
  EXPECT_EQ(c.attribute(QStringLiteral("topic")), QStringLiteral("/missing"));
  EXPECT_EQ(c.attribute(QStringLiteral("field")), QStringLiteral("f"));
}

TEST(RebindCurveKeys, ResolvesXyOnlyWhenBothAxesMatch) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{QStringLiteral("/t"), QStringLiteral("a")};
  const SeriesPath y{QStringLiteral("/t"), QStringLiteral("b")};
  QDomElement both = addXyCurve(pd, x, y);
  QDomElement half = addXyCurve(pd, x, SeriesPath{QStringLiteral("/t"), QStringLiteral("absent")});
  const auto resolve = [&](const SeriesPath& p) -> std::optional<QString> {
    if (p == x) {
      return QStringLiteral("kx");
    }
    if (p == y) {
      return QStringLiteral("ky");
    }
    return std::nullopt;
  };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_EQ(both.attribute(QStringLiteral("curve_x")), QStringLiteral("kx"));
  EXPECT_EQ(both.attribute(QStringLiteral("curve_y")), QStringLiteral("ky"));
  EXPECT_FALSE(half.hasAttribute(QStringLiteral("curve_x")));  // partial → both cleared
  EXPECT_FALSE(half.hasAttribute(QStringLiteral("curve_y")));
  EXPECT_EQ(half.attribute(QStringLiteral("x_topic")), x.topic);
  EXPECT_EQ(half.attribute(QStringLiteral("x_field")), x.field);
  EXPECT_EQ(half.attribute(QStringLiteral("y_topic")), QStringLiteral("/t"));
  EXPECT_EQ(half.attribute(QStringLiteral("y_field")), QStringLiteral("absent"));
  // Only the genuinely-missing half is reported — x resolved, so listing it as
  // "missing" would mislead the prompt.
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{QStringLiteral("/t"), QStringLiteral("absent")}));
}

TEST(RebindCurveKeys, ClearsKeysAndPreservesStableAttrsForUnresolvedXy) {
  PlotDoc pd = makePlotDoc();
  const SeriesPath x{QStringLiteral("/pose"), QStringLiteral("x")};
  const SeriesPath y{QStringLiteral("/pose"), QStringLiteral("y")};
  QDomElement c = addXyCurve(pd, x, y);
  c.setAttribute(QStringLiteral("curve_x"), QStringLiteral("stale_x"));
  c.setAttribute(QStringLiteral("curve_y"), QStringLiteral("stale_y"));

  const auto resolve = [](const SeriesPath&) -> std::optional<QString> { return std::nullopt; };
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(pd.doc, resolve);

  EXPECT_FALSE(c.hasAttribute(QStringLiteral("curve_x")));
  EXPECT_FALSE(c.hasAttribute(QStringLiteral("curve_y")));
  EXPECT_EQ(c.attribute(QStringLiteral("x_topic")), x.topic);
  EXPECT_EQ(c.attribute(QStringLiteral("x_field")), x.field);
  EXPECT_EQ(c.attribute(QStringLiteral("y_topic")), y.topic);
  EXPECT_EQ(c.attribute(QStringLiteral("y_field")), y.field);
  ASSERT_EQ(unresolved.size(), 2);
  EXPECT_EQ(unresolved[0], x);
  EXPECT_EQ(unresolved[1], y);
}

TEST(StripUnresolvedCurves, RemovesOnlyKeylessCurves) {
  PlotDoc pd = makePlotDoc();
  QDomElement keep = addTsCurve(pd, QStringLiteral("/t"), QStringLiteral("a"));
  keep.setAttribute(QStringLiteral("name"), QStringLiteral("resolved_key"));
  addTsCurve(pd, QStringLiteral("/t"), QStringLiteral("b"));  // no name → unresolved
  PJ::layout_xml::stripUnresolvedCurves(pd.doc);
  const QDomNodeList curves = pd.doc.elementsByTagName(QStringLiteral("curve"));
  ASSERT_EQ(curves.size(), 1);
  EXPECT_EQ(curves.at(0).toElement().attribute(QStringLiteral("name")), QStringLiteral("resolved_key"));
}

// ---------- SourceTimelineViewState ----------------------------------------

using PJ::layout_xml::readSourceTimelineViewState;
using PJ::layout_xml::SourceTimelineViewState;
using PJ::layout_xml::writeSourceTimelineViewState;

// write -> serialize -> reparse -> read, so the full XML pipeline (not just the
// in-memory DOM) is exercised.
SourceTimelineViewState roundTripViewState(const SourceTimelineViewState& in) {
  QDomDocument doc;
  doc.appendChild(writeSourceTimelineViewState(doc, in));
  QDomDocument reparsed;
  EXPECT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  return readSourceTimelineViewState(reparsed.documentElement());
}

TEST(SourceTimelineViewState, AllFieldsRoundTrip) {
  SourceTimelineViewState in;
  in.zoom = 1.234567890123456e-7;  // tiny pixels-per-ns: 17 sig-figs must survive
  in.scroll_left_ns = -5'000'000'000LL;
  in.name_column_width = 173;
  in.snap = false;

  const SourceTimelineViewState out = roundTripViewState(in);
  ASSERT_TRUE(out.zoom.has_value());
  EXPECT_DOUBLE_EQ(*out.zoom, *in.zoom);  // exact: 'g',17 preserves the double
  ASSERT_TRUE(out.scroll_left_ns.has_value());
  EXPECT_EQ(*out.scroll_left_ns, *in.scroll_left_ns);
  ASSERT_TRUE(out.name_column_width.has_value());
  EXPECT_EQ(*out.name_column_width, 173);
  ASSERT_TRUE(out.snap.has_value());
  EXPECT_FALSE(*out.snap);
}

TEST(SourceTimelineViewState, AbsentElementYieldsAllNullopt) {
  // A pre-Source-Timeline layout has no <source_timeline> element.
  const SourceTimelineViewState out = readSourceTimelineViewState(QDomElement{});
  EXPECT_FALSE(out.zoom.has_value());
  EXPECT_FALSE(out.scroll_left_ns.has_value());
  EXPECT_FALSE(out.name_column_width.has_value());
  EXPECT_FALSE(out.snap.has_value());
}

TEST(SourceTimelineViewState, MissingAndMalformedAttributesStayNullopt) {
  QDomDocument doc;
  QDomElement el = doc.createElement(QStringLiteral("source_timeline"));
  el.setAttribute(QStringLiteral("zoom"), QStringLiteral("0"));                // non-positive → rejected
  el.setAttribute(QStringLiteral("name_column_width"), QStringLiteral("-4"));  // non-positive → rejected
  el.setAttribute(QStringLiteral("scroll_left_ns"), QStringLiteral("not-a-number"));
  // snap omitted entirely.
  const SourceTimelineViewState out = readSourceTimelineViewState(el);
  EXPECT_FALSE(out.zoom.has_value());
  EXPECT_FALSE(out.name_column_width.has_value());
  EXPECT_FALSE(out.scroll_left_ns.has_value());
  EXPECT_FALSE(out.snap.has_value());
}

TEST(SourceTimelineViewState, UnsetFieldsAreNotWritten) {
  // Only the snap field is set; the element must carry no other attributes, so a
  // partially-populated state never injects bogus zeros on reload.
  SourceTimelineViewState in;
  in.snap = true;
  QDomDocument doc;
  const QDomElement el = writeSourceTimelineViewState(doc, in);
  EXPECT_TRUE(el.hasAttribute(QStringLiteral("snap")));
  EXPECT_FALSE(el.hasAttribute(QStringLiteral("zoom")));
  EXPECT_FALSE(el.hasAttribute(QStringLiteral("scroll_left_ns")));
  EXPECT_FALSE(el.hasAttribute(QStringLiteral("name_column_width")));
}

}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
