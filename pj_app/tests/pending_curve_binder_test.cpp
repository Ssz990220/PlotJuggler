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
#include <string_view>

#include "LayoutXml.h"
#include "PendingCurveBinder.h"
#include "pj_datastore/writer.hpp"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

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
    root = doc.createElement(QStringLiteral("root"));
    doc.appendChild(root);
  }
  QDomElement plot = doc.createElement(QStringLiteral("plot"));
  plot.setAttribute(QStringLiteral("id"), id);
  plot.setAttribute(QStringLiteral("mode"), mode);
  root.appendChild(plot);
  return plot;
}

QDomElement addTimeSeriesCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& path) {
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  curve.setAttribute(QStringLiteral("topic"), path.topic);
  curve.setAttribute(QStringLiteral("field"), path.field);
  plot.appendChild(curve);
  return curve;
}

QDomElement addXyCurve(QDomDocument& doc, QDomElement& plot, const SeriesPath& x_path, const SeriesPath& y_path) {
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  curve.setAttribute(QStringLiteral("x_topic"), x_path.topic);
  curve.setAttribute(QStringLiteral("x_field"), x_path.field);
  curve.setAttribute(QStringLiteral("y_topic"), y_path.topic);
  curve.setAttribute(QStringLiteral("y_field"), y_path.field);
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

TEST(PendingCurveBinderTest, CollectsUnresolvedCurvesAndBindsThemWhenTopicArrives) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath ready{QStringLiteral("/ready"), QStringLiteral("value")};
  const SeriesPath late{QStringLiteral("/late"), QStringLiteral("value")};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, ready.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, QStringLiteral("plot_ts"), QStringLiteral("TimeSeries"));
  addTimeSeriesCurve(doc, plot_element, ready);
  QDomElement late_curve = addTimeSeriesCurve(doc, plot_element, late);
  late_curve.setAttribute(QStringLiteral("color"), QStringLiteral("#123456"));
  late_curve.setAttribute(QStringLiteral("line_width"), QStringLiteral("3.50"));
  late_curve.setAttribute(QStringLiteral("style"), QStringLiteral("Dots"));
  late_curve.setAttribute(QStringLiteral("visible"), QStringLiteral("false"));
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_EQ(plot.curveList().size(), 1U);

  PJ::PendingCurveBinder binder(app_session.catalogModel());
  binder.collect(doc, indexByStateId(plot));
  ASSERT_EQ(binder.size(), 1);
  ASSERT_EQ(binder.unresolved().size(), 1);
  EXPECT_EQ(binder.unresolved().front(), late);
  EXPECT_EQ(binder.flush(QSet<QString>{QStringLiteral("/unrelated")}), 0);
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
  EXPECT_EQ(info->curve->pen().color(), QColor(QStringLiteral("#123456")));
  EXPECT_DOUBLE_EQ(info->curve->pen().widthF(), 3.5);
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Dots);
  EXPECT_FALSE(info->curve->isVisible());

  EXPECT_EQ(binder.flush(QSet<QString>{late.topic}), 0);
  EXPECT_EQ(plot.curveList().size(), 2U);
}

TEST(PendingCurveBinderTest, XyCurveWaitsForBothHalvesBeforeBinding) {
  QTemporaryDir extensions_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  PJ::AppSession app_session(extensions_dir.path());
  const PJ::DatasetId dataset_id = createDataset(app_session);
  ASSERT_NE(dataset_id, 0U);

  const SeriesPath x_path{QStringLiteral("/pose_x"), QStringLiteral("value")};
  const SeriesPath y_path{QStringLiteral("/pose_y"), QStringLiteral("value")};
  ASSERT_NE(addScalarTopic(app_session, dataset_id, x_path.topic.toStdString()), 0U);

  QDomDocument doc;
  QDomElement plot_element = addPlot(doc, QStringLiteral("plot_xy"), QStringLiteral("XYPlot"));
  addXyCurve(doc, plot_element, x_path, y_path);
  rebindAgainstCatalog(doc, app_session.catalogModel());

  PJ::PlotWidget plot(&app_session.sessionManager(), &app_session.catalogModel());
  ASSERT_TRUE(plot.xmlLoadState(plot_element));
  ASSERT_TRUE(plot.curveList().empty());

  PJ::PendingCurveBinder binder(app_session.catalogModel());
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
