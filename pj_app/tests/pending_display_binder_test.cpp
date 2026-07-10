// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QColor>
#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QPen>
#include <QSet>
#include <QTemporaryDir>
#include <QtGlobal>
#include <algorithm>
#include <memory>
#include <string_view>

#include "LayoutXml.h"
#include "PendingDisplayBinder.h"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/TopicDemandTracker.h"
using namespace Qt::StringLiterals;

namespace {

using PJ::layout_xml::SeriesPath;

PJ::DatasetId createDataset(PJ::AppSession& app_session) {
  auto dataset = app_session.sessionManager().dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "test"});
  if (!dataset.has_value()) {
    ADD_FAILURE() << dataset.error();
    return 0;
  }
  return *dataset;
}

PJ::TopicId addScalarTopic(PJ::AppSession& app_session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = app_session.sessionManager().dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  if (!handle.has_value()) {
    ADD_FAILURE() << handle.error();
    return 0;
  }
  writer.appendScalar(*handle, 100, 1.0);
  writer.appendScalar(*handle, 200, 2.0);
  if (app_session.sessionManager().commitChunks(writer.flushAll()).empty()) {
    ADD_FAILURE() << "commit produced no changed topics";
    return 0;
  }
  app_session.catalogModel().rebuildFromDatastore();
  return handle->topic_id;
}

QDomElement addPlot(QDomDocument& doc, const QString& id, const QString& mode) {
  QDomElement root = doc.documentElement();
  if (root.isNull()) {
    root = doc.createElement(u"root"_s);
    doc.appendChild(root);
  }
  QDomElement plot = doc.createElement(u"plot"_s);
  plot.setAttribute(u"id"_s, id);
  plot.setAttribute(u"mode"_s, mode);
  root.appendChild(plot);
  return plot;
}

QDomElement addTimeSeriesCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& path) {
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"topic"_s, path.topic);
  curve.setAttribute(u"field"_s, path.field);
  plot.appendChild(curve);
  return curve;
}

QDomElement addXyCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& x_path, const SeriesPath& y_path) {
  QDomElement curve = doc.createElement(u"curve"_s);
  curve.setAttribute(u"x_topic"_s, x_path.topic);
  curve.setAttribute(u"x_field"_s, x_path.field);
  curve.setAttribute(u"y_topic"_s, y_path.topic);
  curve.setAttribute(u"y_field"_s, y_path.field);
  plot.appendChild(curve);
  return curve;
}

void rebindAgainstCatalog(QDomDocument& doc, PJ::CatalogModel& catalog) {
  const QList<SeriesPath> unresolved = PJ::layout_xml::rebindCurveKeys(
      doc, [&catalog](const SeriesPath& path) { return PJ::resolveSeriesPath(catalog, path); });
  (void)unresolved;
}

QHash<QString, PJ::PlotWidget*> indexByStateId(PJ::PlotWidget& plot) {
  QHash<QString, PJ::PlotWidget*> plots;
  plots.insert(plot.stateId(), &plot);
  return plots;
}

}  // namespace

TEST(PendingDisplayBinderTest, RestoredEntryReleasesItsReferenceWhenThePlotDies) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  PJ::TopicDemandTracker tracker;
  PJ::PendingDisplayBinder binder(app_session.catalogModel(), &tracker);

  // The topic is advertised (nameable) but has no data — a layout restore over
  // a live demand stream stages a pending curve holding a demand reference.
  app_session.catalogModel().setAdvertisedTopics(
      dataset_id, {PJ::AdvertisedTopic{u"/speed"_s, PJ::sdk::BuiltinObjectType::kNone}});
  QDomDocument doc;
  auto plot = std::make_unique<PJ::PlotWidget>(&app_session.sessionManager(), &app_session.catalogModel());
  QDomElement plot_element = addPlot(doc, plot->stateId(), u"time"_s);
  addTimeSeriesCurve(doc, plot_element, SeriesPath{u"/speed"_s, u"value"_s});
  binder.collect(doc, indexByStateId(*plot));
  ASSERT_EQ(binder.size(), 1);
  const auto held = tracker.activeTopics(dataset_id);
  ASSERT_NE(std::find(held.begin(), held.end(), u"/speed"_s), held.end());

  // The plot dies before the topic ever materializes: the reference must
  // release AT destruction — on a quiet stream no later flush ever runs, and a
  // lazy release would keep the topic subscribed forever.
  plot.reset();
  const auto after = tracker.activeTopics(dataset_id);
  EXPECT_EQ(std::find(after.begin(), after.end(), u"/speed"_s), after.end());
  EXPECT_EQ(binder.size(), 0);
}

TEST(PendingDisplayBinderTest, DuplicatePlaceholderDropStagesOneEntry) {
  PJ::AppSession app_session;
  const PJ::DatasetId dataset_id = createDataset(app_session);
  PJ::TopicDemandTracker tracker;
  PJ::PendingDisplayBinder binder(app_session.catalogModel(), &tracker);
  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());

  // Double-dropping the same placeholder on the same plot must not stage a
  // second intent: on promotion the first entry binds every field, the
  // duplicate's addCurve() calls all return null, and it would sit forever
  // holding its demand reference (and re-add ghost curves after a manual
  // curve delete on a later flush).
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_id);
  binder.addPendingCurve(&plot, SeriesPath{u"/speed"_s, QString()}, dataset_id);
  EXPECT_EQ(binder.size(), 1);
}

TEST(PendingDisplayBinderTest, CollectsUnresolvedCurvesAndBindsThemWhenTopicArrives) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath ready{u"/ready"_s, u"value"_s};
  const SeriesPath late{u"/late"_s, u"value"_s};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, ready.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_ts"_s, u"TimeSeries"_s);
  // Style and line width are plot-level (every curve inherits them); colour and
  // visibility are per-curve.
  plot_element.setAttribute(u"style"_s, u"Dots"_s);
  plot_element.setAttribute(u"line_width"_s, u"3.0"_s);
  addTimeSeriesCurve(doc, plot_element, ready);
  QDomElement late_curve = addTimeSeriesCurve(doc, plot_element, late);
  late_curve.setAttribute(u"color"_s, u"#123456"_s);
  late_curve.setAttribute(u"visible"_s, u"false"_s);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_EQ(plot.curveList().size(), 1U);

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  ASSERT_EQ(binder.unresolved().size(), 1);
  EXPECT_EQ(binder.unresolved().front(), late);
  EXPECT_EQ(binder.flush(QSet<QString>{u"/unrelated"_s}), 0);
  EXPECT_EQ(binder.size(), 1);

  ASSERT_NE(addScalarTopic(app_session, dataset_id, late.topic.toStdString()), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{late.topic}), 1);
  EXPECT_TRUE(binder.empty());
  ASSERT_EQ(plot.curveList().size(), 2U);

  const auto late_key = PJ::resolveSeriesPath(app_session.catalogModel(), late);
  ASSERT_TRUE(late_key.has_value());
  PJ::PlotWidget::CurveInfo* info = plot.curveFromTitle(*late_key);
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->curve, nullptr);
  EXPECT_EQ(info->curve->pen().color(), QColor(u"#123456"_s));
  // Style and width are inherited from the plot (Dots at the plot's line width).
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Dots);
  EXPECT_DOUBLE_EQ(info->curve->pen().widthF(), PJ::dotWidthValue(PJ::LineWidth::kPoints30));
  EXPECT_FALSE(info->curve->isVisible());

  EXPECT_EQ(binder.flush(QSet<QString>{late.topic}), 0);
  EXPECT_EQ(plot.curveList().size(), 2U);
}

TEST(PendingDisplayBinderTest, XyCurveWaitsForBothHalvesBeforeBinding) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath x_path{u"/pose_x"_s, u"value"_s};
  const SeriesPath y_path{u"/pose_y"_s, u"value"_s};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, x_path.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, u"plot_xy"_s, u"XYPlot"_s);
  addXyCurve(doc, plot_element, x_path, y_path);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_TRUE(plot.curveList().empty());

  PJ::PendingDisplayBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  // x is already in the catalog, so only the missing half (y) is reported.
  const QList<SeriesPath> unresolved = binder.unresolved();
  ASSERT_EQ(unresolved.size(), 1);
  EXPECT_EQ(unresolved[0], y_path);

  EXPECT_EQ(binder.flush(QSet<QString>{x_path.topic}), 0);
  EXPECT_EQ(binder.size(), 1);
  EXPECT_TRUE(plot.curveList().empty());

  ASSERT_NE(addScalarTopic(app_session, dataset_id, y_path.topic.toStdString()), 0U);
  EXPECT_EQ(binder.flush(QSet<QString>{y_path.topic}), 1);
  EXPECT_TRUE(binder.empty());
  EXPECT_EQ(plot.curveList().size(), 1U);

  EXPECT_EQ(binder.flush({}), 0);
  EXPECT_EQ(plot.curveList().size(), 1U);
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
