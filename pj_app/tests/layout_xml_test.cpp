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

using PJ::LayoutXml::DataSourceRef;

// ---------- appendJsonAsCdata ----------------------------------------------

QString roundTripJson(const QString& input) {
  QDomDocument doc;
  QDomElement plugin = doc.createElement(QStringLiteral("plugin"));
  PJ::LayoutXml::appendJsonAsCdata(doc, plugin, input);
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
    PJ::LayoutXml::appendJsonAsCdata(doc, plugin, plugin_json);
    file_info.appendChild(plugin);
  }
  wrapper.appendChild(file_info);
  return doc;
}

TEST(ExtractDataSource, EmptyDocReturnsEmptyRef) {
  QDomDocument doc;
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(ref.resolved_path.isEmpty());
  EXPECT_TRUE(ref.plugin_id.isEmpty());
  EXPECT_TRUE(ref.plugin_config_json.isEmpty());
}

TEST(ExtractDataSource, MissingWrapperReturnsEmptyRef) {
  QDomDocument doc;
  doc.appendChild(doc.createElement(QStringLiteral("root")));
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(ref.resolved_path.isEmpty());
}

TEST(ExtractDataSource, EmptyFilenameAttributeReturnsEmpty) {
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral(""));
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir::current());
  EXPECT_TRUE(ref.resolved_path.isEmpty());
}

TEST(ExtractDataSource, AbsolutePathPassesThrough) {
  // Build a genuinely-absolute path for the host platform. A hardcoded POSIX
  // path like "/tmp/x" is drive-relative on Windows, so QFileInfo would anchor
  // it at the current drive and the equality check would fail.
  const QString abs = QDir::tempPath() + QStringLiteral("/some_data.mcap");
  const QDomDocument doc = buildDataSourceDoc(abs);
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir(QDir::rootPath()));
  EXPECT_EQ(ref.resolved_path, QFileInfo(abs).absoluteFilePath());
}

TEST(ExtractDataSource, RelativePathIsAnchoredAtLayoutDir) {
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral("data/run.csv"));
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir(QStringLiteral("/tmp/layouts")));
  EXPECT_EQ(ref.resolved_path, QStringLiteral("/tmp/layouts/data/run.csv"));
}

TEST(ExtractDataSource, PluginIdAndCdataJsonRoundTrip) {
  const QString json = QStringLiteral(R"({"topics":["a","b"]})");
  const QDomDocument doc =
      buildDataSourceDoc(QStringLiteral("/tmp/x.mcap"), QStringLiteral("robot"), QStringLiteral("DataLoad MCAP"), json);
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(doc, QDir::current());
  EXPECT_EQ(ref.prefix, QStringLiteral("robot"));
  EXPECT_EQ(ref.plugin_id, QStringLiteral("DataLoad MCAP"));
  EXPECT_EQ(ref.plugin_config_json, json);
}

TEST(ExtractDataSource, PluginCdataWithClosingSequenceRoundTrips) {
  const QString json = QStringLiteral(R"({"pat":"weird ]]> in middle"})");
  const QDomDocument doc = buildDataSourceDoc(QStringLiteral("/tmp/x.mcap"), QString(), QStringLiteral("CSV"), json);
  // Round-trip the WHOLE doc through serialize+reparse to confirm the
  // CDATA splitting survives the actual file pipeline.
  QDomDocument reparsed;
  ASSERT_TRUE(reparsed.setContent(doc.toByteArray(2)));
  const DataSourceRef ref = PJ::LayoutXml::extractDataSource(reparsed, QDir::current());
  EXPECT_EQ(ref.plugin_config_json, json);
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
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(QString(), QString()));
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(QStringLiteral("/tmp/x"), QString()));
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(QString(), QStringLiteral("/tmp/x")));
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
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(a, b));
}

TEST(IsSamePath, IdenticalAbsolutePathsAreSame) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString p = dir.filePath(QStringLiteral("log.mcap"));
  ASSERT_TRUE(QFile(p).open(QIODevice::WriteOnly));
  EXPECT_TRUE(PJ::LayoutXml::isSamePath(p, p));
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
  EXPECT_TRUE(PJ::LayoutXml::isSamePath(abs, QStringLiteral("log.mcap")));
  EXPECT_TRUE(QDir::setCurrent(cwd_before));
}

TEST(IsSamePath, NonexistentPathsAreNotSame) {
  // canonicalFilePath() returns empty for missing files; treating those
  // as "same" would mean two layouts that reference deleted files would
  // skip the reload prompt — the opposite of helpful.
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(QStringLiteral("/nonexistent/a.mcap"), QStringLiteral("/nonexistent/b.mcap")));
  EXPECT_FALSE(PJ::LayoutXml::isSamePath(QStringLiteral("/nonexistent/a.mcap"), QStringLiteral("/nonexistent/a.mcap")));
}

TEST(EnsureLayoutExtension, AppendsWhenNoExtension) {
  EXPECT_EQ(PJ::LayoutXml::ensureLayoutExtension(QStringLiteral("my_layout")), QStringLiteral("my_layout.pj4.xml"));
  EXPECT_EQ(
      PJ::LayoutXml::ensureLayoutExtension(QStringLiteral("/home/user/setup")),
      QStringLiteral("/home/user/setup.pj4.xml"));
}

TEST(EnsureLayoutExtension, LeavesCorrectExtensionUntouched) {
  EXPECT_EQ(
      PJ::LayoutXml::ensureLayoutExtension(QStringLiteral("my_layout.pj4.xml")), QStringLiteral("my_layout.pj4.xml"));
}

TEST(EnsureLayoutExtension, RespectsAnyUserSpecifiedExtension) {
  // Contract is "append only when no extension is specified", so a name the
  // user deliberately gave another suffix is left alone rather than turned
  // into a double extension like notes.txt.pj4.xml.
  EXPECT_EQ(PJ::LayoutXml::ensureLayoutExtension(QStringLiteral("notes.txt")), QStringLiteral("notes.txt"));
  EXPECT_EQ(PJ::LayoutXml::ensureLayoutExtension(QStringLiteral("my.layout")), QStringLiteral("my.layout"));
}

TEST(EnsureLayoutExtension, EmptyInEmptyOut) {
  EXPECT_EQ(PJ::LayoutXml::ensureLayoutExtension(QString()), QString());
}

// ---------- SeriesPath / extractSeriesPaths / rebindCurveKeys ---------------

using PJ::LayoutXml::SeriesPath;

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
  const QList<SeriesPath> paths = PJ::LayoutXml::extractSeriesPaths(pd.doc);
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
  const QList<SeriesPath> paths = PJ::LayoutXml::extractSeriesPaths(pd.doc);
  ASSERT_EQ(paths.size(), 2);
  EXPECT_EQ(paths[0], x);
  EXPECT_EQ(paths[1], y);
}

TEST(ExtractSeriesPaths, SkipsCurvesWithoutStableIdentity) {
  PlotDoc pd = makePlotDoc();
  QDomElement legacy = pd.doc.createElement(QStringLiteral("curve"));
  legacy.setAttribute(QStringLiteral("name"), QStringLiteral("dataset:1/topic:2/column:0"));
  pd.plot.appendChild(legacy);
  EXPECT_TRUE(PJ::LayoutXml::extractSeriesPaths(pd.doc).isEmpty());
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
  const QList<SeriesPath> unresolved = PJ::LayoutXml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_TRUE(unresolved.isEmpty());
  EXPECT_EQ(c.attribute(QStringLiteral("name")), QStringLiteral("dataset:7/topic:3/column:0"));
}

TEST(RebindCurveKeys, ClearsNameAndReportsUnresolvedTimeSeries) {
  PlotDoc pd = makePlotDoc();
  QDomElement c = addTsCurve(pd, QStringLiteral("/missing"), QStringLiteral("f"));
  c.setAttribute(QStringLiteral("name"), QStringLiteral("stale_key"));  // stale from prior session
  const auto resolve = [](const SeriesPath&) -> std::optional<QString> { return std::nullopt; };
  const QList<SeriesPath> unresolved = PJ::LayoutXml::rebindCurveKeys(pd.doc, resolve);
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{QStringLiteral("/missing"), QStringLiteral("f")}));
  EXPECT_FALSE(c.hasAttribute(QStringLiteral("name")));  // stale key cleared, won't mis-resolve
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
  const QList<SeriesPath> unresolved = PJ::LayoutXml::rebindCurveKeys(pd.doc, resolve);
  EXPECT_EQ(both.attribute(QStringLiteral("curve_x")), QStringLiteral("kx"));
  EXPECT_EQ(both.attribute(QStringLiteral("curve_y")), QStringLiteral("ky"));
  EXPECT_FALSE(half.hasAttribute(QStringLiteral("curve_x")));  // partial → both cleared
  EXPECT_FALSE(half.hasAttribute(QStringLiteral("curve_y")));
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], (SeriesPath{QStringLiteral("/t"), QStringLiteral("absent")}));
}

TEST(StripUnresolvedCurves, RemovesOnlyKeylessCurves) {
  PlotDoc pd = makePlotDoc();
  QDomElement keep = addTsCurve(pd, QStringLiteral("/t"), QStringLiteral("a"));
  keep.setAttribute(QStringLiteral("name"), QStringLiteral("resolved_key"));
  addTsCurve(pd, QStringLiteral("/t"), QStringLiteral("b"));  // no name → unresolved
  PJ::LayoutXml::stripUnresolvedCurves(pd.doc);
  const QDomNodeList curves = pd.doc.elementsByTagName(QStringLiteral("curve"));
  ASSERT_EQ(curves.size(), 1);
  EXPECT_EQ(curves.at(0).toElement().attribute(QStringLiteral("name")), QStringLiteral("resolved_key"));
}

}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
