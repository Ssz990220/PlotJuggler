// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>

#include <QApplication>
#include <QDomDocument>
#include <QPen>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/PlotWidgetBase.h"
#include "pj_plotting/XYCurveDialog.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/CurveDescriptor.h"
#include "pj_runtime/SessionManager.h"

namespace {

PJ::TopicId addScalarTopic(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle_or = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle_or.has_value()) << handle_or.error();
  if (!handle_or.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle_or, 100, 1.0);
  writer.appendScalar(*handle_or, 200, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle_or->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

}  // namespace

// Style and width are plot-level properties: changing one restyles/re-pens every
// existing curve AND is inherited by curves added afterwards. (Two curves in one
// plot can never differ in style/width — only colour is per-curve.)
TEST(PlotWidgetCurveStyle, StyleAndWidthAreInheritedByNewCurves) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_b = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  const QString key_c = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/z"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  auto* info_a = plot.addCurve(key_a);
  ASSERT_NE(info_a, nullptr);
  QwtPlotCurve* curve_a = info_a->curve;  // QwtPlotCurve* is heap-stable across later addCurve() reallocs.
  ASSERT_NE(curve_a, nullptr);
  ASSERT_EQ(curve_a->style(), QwtPlotCurve::Lines);

  // Plot-level style change restyles the existing curve...
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);
  EXPECT_EQ(curve_a->style(), QwtPlotCurve::Dots);
  // ...and is inherited by a curve added afterwards.
  auto* info_b = plot.addCurve(key_b);
  ASSERT_NE(info_b, nullptr);
  ASSERT_NE(info_b->curve, nullptr);
  EXPECT_EQ(info_b->curve->style(), QwtPlotCurve::Dots);

  // Same for width: existing and new curves share the plot's line width.
  plot.setDefaultStyle(PJ::PlotWidgetBase::kLines);
  plot.setLineWidth(PJ::LineWidth::kPoints30);
  const double expected_width = PJ::lineWidthValue(PJ::LineWidth::kPoints30);
  EXPECT_DOUBLE_EQ(curve_a->pen().widthF(), expected_width);
  auto* info_c = plot.addCurve(key_c);
  ASSERT_NE(info_c, nullptr);
  ASSERT_NE(info_c->curve, nullptr);
  EXPECT_DOUBLE_EQ(info_c->curve->pen().widthF(), expected_width);
}

// A point-only (Dots) curve draws its dots at dotWidthValue, so they are as
// visible as the dots of a Lines+Dots curve — not 1px specks. (Regression for
// the old per-curve style toggle that left the thin line pen in place.)
TEST(PlotWidgetCurveStyle, DotsUseDotWidth) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);
  auto* info_a = plot.addCurve(key_a);
  ASSERT_NE(info_a, nullptr);
  ASSERT_NE(info_a->curve, nullptr);
  EXPECT_EQ(info_a->curve->style(), QwtPlotCurve::Dots);
  EXPECT_DOUBLE_EQ(info_a->curve->pen().widthF(), PJ::dotWidthValue(PJ::LineWidth::kPoints10));
}

// Style is a plot-level property, so it is serialized once on the <plot>, never
// per <curve> (curves carry only colour + visibility).
TEST(PlotWidgetCurveStyle, StyleIsSavedOnThePlotNotPerCurve) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_a = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  ASSERT_FALSE(key_a.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  ASSERT_NE(plot.addCurve(key_a), nullptr);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);

  QDomDocument doc;
  const QDomElement element = plot.xmlSaveState(doc);
  EXPECT_EQ(element.attribute(QStringLiteral("style")), QStringLiteral("Dots"));
  const QDomElement curve = element.firstChildElement(QStringLiteral("curve"));
  ASSERT_FALSE(curve.isNull());
  EXPECT_FALSE(curve.hasAttribute(QStringLiteral("style")));
  EXPECT_FALSE(curve.hasAttribute(QStringLiteral("line_width")));
}

// XY (scatter) plots default to Dots, and a plot-level style/width change must
// NOT downgrade them to Lines. Regression: making style/width plot-level means
// every plot-level restyle runs over XY curves too. XY curves are NOT forced to a
// style — they use the plot-level style like any curve (the Curve Style toolbar
// controls them), and a later plot-level restyle keeps them on that style.
TEST(PlotWidgetCurveStyle, XyPlotUsesPlotLevelStyle) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  ASSERT_FALSE(key_x.isEmpty());
  ASSERT_FALSE(key_y.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kSticks);  // the user's chosen style

  auto* info = plot.addCurveXY(key_x, key_y, QStringLiteral("x vs y"));
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->curve, nullptr);
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Sticks);  // XY uses the plot style, not forced Dots

  // A plot-level width change re-applies the plot style to every curve, XY included.
  plot.setLineWidth(PJ::LineWidth::kPoints30);
  EXPECT_EQ(info->curve->style(), QwtPlotCurve::Sticks);
}

// Older PJ4 layouts kept the plot-level line_width at the stale default and stored
// the real width per-curve. Loading one must recover the width from the first curve
// instead of rendering everything thin.
TEST(PlotWidgetCurveStyle, OldLayoutRecoversLineWidthFromFirstCurve) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  PJ::PlotWidget plot(&session, &catalog);

  QDomDocument doc;
  QDomElement plot_el = doc.createElement(QStringLiteral("plot"));
  plot_el.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  plot_el.setAttribute(QStringLiteral("line_width"), QStringLiteral("1.0"));  // stale plot-level default
  QDomElement curve_el = doc.createElement(QStringLiteral("curve"));
  // The curve key need not resolve — the width is read from the element before curves load.
  curve_el.setAttribute(QStringLiteral("name"), QStringLiteral("dataset:1/topic:999/column:0"));
  curve_el.setAttribute(QStringLiteral("line_width"), QStringLiteral("3.00"));  // real old per-curve width
  plot_el.appendChild(curve_el);

  plot.xmlLoadState(plot_el);
  EXPECT_EQ(plot.lineWidth(), PJ::LineWidth::kPoints30);
}

// The XY dialog's alias auto-suggestion: common prefix + "[suffixX;suffixY]".
TEST(XYCurveDialog, SuggestAlias) {
  EXPECT_EQ(
      PJ::XYCurveDialog::suggestAlias(QStringLiteral("/imu/x"), QStringLiteral("/imu/y")),
      QStringLiteral("/imu/[x;y]"));
  EXPECT_EQ(PJ::XYCurveDialog::suggestAlias(QStringLiteral("abc"), QStringLiteral("xyz")), QStringLiteral("[abc;xyz]"));
}

// A plot can hold multiple XY curves; each carries its user alias as the title,
// renders as Dots, and saves the alias (the title is not derivable from x/y).
TEST(PlotWidgetCurveStyle, MultipleXyCurvesCarryAliasAndDots) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  const QString key_z = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/z"));
  ASSERT_FALSE(key_x.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);  // the user picks Dots for the scatter
  auto* c1 = plot.addCurveXY(key_x, key_y, QStringLiteral("x vs y"));
  auto* c2 = plot.addCurveXY(key_x, key_z, QStringLiteral("x vs z"));
  ASSERT_NE(c1, nullptr);
  ASSERT_NE(c2, nullptr);
  ASSERT_EQ(plot.curveList().size(), 2U);
  EXPECT_EQ(c1->source_name, QStringLiteral("x vs y"));
  EXPECT_EQ(c2->source_name, QStringLiteral("x vs z"));
  EXPECT_EQ(c1->curve->style(), QwtPlotCurve::Dots);  // both inherit the plot style
  EXPECT_EQ(c2->curve->style(), QwtPlotCurve::Dots);

  // Save: each XY curve persists its alias (name attr).
  QDomDocument doc;
  const QDomElement element = plot.xmlSaveState(doc);
  int xy_curves = 0;
  for (QDomElement c = element.firstChildElement(QStringLiteral("curve")); !c.isNull();
       c = c.nextSiblingElement(QStringLiteral("curve"))) {
    EXPECT_TRUE(c.hasAttribute(QStringLiteral("name")));
    ++xy_curves;
  }
  EXPECT_EQ(xy_curves, 2);
}

// Loading an XY curve restores its saved alias and the plot style WITHOUT a dialog
// (applyCurveElement passes the saved name straight to addCurveXY; xmlLoadState has
// already seeded the plot-level style, simulated here by setDefaultStyle).
TEST(PlotWidgetCurveStyle, XyLoadRestoresAliasWithoutDialog) {
  PJ::SessionManager session;
  PJ::CatalogModel catalog(&session);
  auto dataset = session.dataEngine().createDataset(PJ::DatasetDescriptor{.source_name = "drive.mcap"});
  ASSERT_TRUE(dataset.has_value()) << dataset.error();
  const QString key_x = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/x"));
  const QString key_y = keyForTopic(catalog, addScalarTopic(session, *dataset, "/imu/y"));
  ASSERT_FALSE(key_x.isEmpty());

  PJ::PlotWidget plot(&session, &catalog);
  plot.setModeXY(true);
  plot.setDefaultStyle(PJ::PlotWidgetBase::kDots);  // the plot's restored style

  QDomDocument doc;
  QDomElement xy_el = doc.createElement(QStringLiteral("curve"));
  xy_el.setAttribute(QStringLiteral("curve_x"), key_x);  // post-rebind keys
  xy_el.setAttribute(QStringLiteral("curve_y"), key_y);
  xy_el.setAttribute(QStringLiteral("name"), QStringLiteral("my alias"));

  auto* loaded = plot.applyCurveElement(xy_el);
  ASSERT_NE(loaded, nullptr);
  ASSERT_NE(loaded->curve, nullptr);
  EXPECT_EQ(loaded->source_name, QStringLiteral("my alias"));
  EXPECT_EQ(loaded->curve->style(), QwtPlotCurve::Dots);  // inherits the plot style
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
