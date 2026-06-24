// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The plot's saved X-axis (time) range must be persisted in ABSOLUTE time, not
// in the display-relative seconds the Qwt axis speaks. PJ4's display offset is
// per-dataset and live (it tracks the "Use time offset" toggle and each
// dataset's earliest sample), so a range saved in display coordinates only
// frames the right window for the exact offset present at save time. Storing
// absolute time makes a restored range correct regardless of the offset state
// at load (different toggle position, reloaded data, a shared layout).

#include <gtest/gtest.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QRectF>
#include <QtGlobal>
#include <string_view>

#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace {

// Samples sit at a realistic absolute epoch (~2020), so the per-dataset "Use
// time offset" shift is ~1.6e9 s -- large enough that a display-relative range
// ([0, 1]) and the absolute range ([1.6e9, 1.6e9 + 1]) are wildly different
// numbers. That gap is exactly what the bug under test produces.
constexpr PJ::Timestamp kT0Ns = 1'600'000'000'000'000'000;  // 1.6e9 s, in ns
constexpr PJ::Timestamp kOneSecondNs = 1'000'000'000;
constexpr double kT0Sec = 1'600'000'000.0;

PJ::TopicId addScalarTopicAtEpoch(PJ::SessionManager& session, PJ::DatasetId dataset_id, std::string_view topic_name) {
  PJ::DataWriter writer = session.dataEngine().createWriter();
  auto handle = writer.registerScalarSeries(dataset_id, topic_name, PJ::NumericType::kFloat64);
  EXPECT_TRUE(handle.has_value()) << handle.error();
  if (!handle.has_value()) {
    return 0;
  }
  writer.appendScalar(*handle, kT0Ns, 1.0);
  writer.appendScalar(*handle, kT0Ns + kOneSecondNs, 2.0);
  EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
  return handle->topic_id;
}

QString keyForTopic(PJ::CatalogModel& catalog, PJ::TopicId topic_id) {
  for (const auto& curve : catalog.curves()) {
    if (const auto descriptor = catalog.curveDescriptor(curve.name); descriptor && descriptor->topic_id == topic_id) {
      return curve.name;
    }
  }
  return {};
}

// Mirror MainWindow's pre-load rebind (layout_xml::rebindCurveKeys): the saved
// <curve> carries topic/field, but xmlLoadState matches/creates curves by the
// "name" attribute. Setting it keeps the curve across the load so the plot can
// resolve the dataset's display offset.
void rebindCurveNames(QDomElement& plot_element, const QString& key) {
  for (QDomElement curve = plot_element.firstChildElement(QStringLiteral("curve")); !curve.isNull();
       curve = curve.nextSiblingElement(QStringLiteral("curve"))) {
    curve.setAttribute(QStringLiteral("name"), key);
  }
}

// A plot with one scalar curve drawn from a dataset at the absolute epoch above.
struct PlotFixture {
  PJ::SessionManager session;
  PJ::CatalogModel catalog{&session};
  PJ::PlotWidget plot{&session, &catalog};
  QString key;

  PlotFixture() {
    // Own TimeDomain (mirrors FileLoader's one-domain-per-source): "Use time
    // offset" writes the align-starts shift to the domain, so a default (id 0)
    // domain would carry no offset and the offset-on assertions could not hold.
    auto domain = session.dataEngine().createTimeDomain("drive");
    EXPECT_TRUE(domain.has_value()) << domain.error();
    auto dataset = session.dataEngine().createDataset(
        PJ::DatasetDescriptor{.source_name = "drive.mcap", .time_domain_id = *domain});
    EXPECT_TRUE(dataset.has_value()) << dataset.error();
    const PJ::TopicId topic = addScalarTopicAtEpoch(session, *dataset, "/imu/accel");
    key = keyForTopic(catalog, topic);
    EXPECT_FALSE(key.isEmpty());
    EXPECT_NE(plot.addCurve(key), nullptr);
  }
};

}  // namespace

// With "Use time offset" ON, the axis frames display window [0, 1] s. Saving must
// record the ABSOLUTE window [1.6e9, 1.6e9 + 1] s. The layout always stores a time
// axis absolute (the offset is a visualization concern), so NO x_absolute marker is
// written — the plot mode distinguishes time-series from XY on load.
TEST(PlotWidgetRangeOffset, SavesAbsoluteTimeRange) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);  // display offset = dataset min = kT0Ns
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  const QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  const QDomElement range = plot_element.firstChildElement(QStringLiteral("range"));
  ASSERT_FALSE(range.isNull());

  EXPECT_FALSE(range.hasAttribute(QStringLiteral("x_absolute"))) << "the obsolete marker must not be written";
  EXPECT_NEAR(range.attribute(QStringLiteral("left")).toDouble(), kT0Sec, 1e-3);
  EXPECT_NEAR(range.attribute(QStringLiteral("right")).toDouble(), kT0Sec + 1.0, 1e-3);
}

// The core bug: save with the offset ON, load with it OFF. The restored axis
// must still frame the same ABSOLUTE instant. With the offset now 0, display ==
// absolute, so the axis reads [1.6e9, 1.6e9 + 1].
TEST(PlotWidgetRangeOffset, RestoresAbsoluteWindowAcrossOffsetChange) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  rebindCurveNames(plot_element, fixture.key);

  fixture.session.setUseTimeOffset(false);  // load-time offset = 0
  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), kT0Sec, 1e-3);
  EXPECT_NEAR(rect.right(), kT0Sec + 1.0, 1e-3);
}

// Common reload case: offset ON at save AND load. The restored axis reproduces
// the same DISPLAY window [0, 1] (absolute round-tripped back through the same
// offset). Exercises the curve-present representative-offset path on load.
TEST(PlotWidgetRangeOffset, RestoresDisplayWindowWhenOffsetUnchanged) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);
  fixture.plot.setZoomRectangle(QRectF(0.0, -5.0, 1.0, 10.0), /*emit_signal=*/false);

  QDomDocument doc;
  QDomElement plot_element = fixture.plot.xmlSaveState(doc);
  rebindCurveNames(plot_element, fixture.key);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), 0.0, 1e-3);
  EXPECT_NEAR(rect.right(), 1.0, 1e-3);
}

// A layout's time-axis <range> is ALWAYS absolute seconds — including legacy layouts
// that predate the (now-removed) x_absolute marker. An unmarked time-series range is
// treated as absolute and converted with the current offset, NOT loaded verbatim. Here
// an absolute window [kT0Sec, kT0Sec + 1] with no marker restores to display [0, 1].
TEST(PlotWidgetRangeOffset, UnmarkedTimeRangeLoadsAsAbsolute) {
  PlotFixture fixture;
  fixture.session.setUseTimeOffset(true);  // offset = kT0Ns

  QDomDocument doc;
  QDomElement plot_element = doc.createElement(QStringLiteral("plot"));
  plot_element.setAttribute(QStringLiteral("mode"), QStringLiteral("TimeSeries"));
  QDomElement range = doc.createElement(QStringLiteral("range"));
  range.setAttribute(QStringLiteral("left"), QString::number(kT0Sec, 'f', 6));
  range.setAttribute(QStringLiteral("right"), QString::number(kT0Sec + 1.0, 'f', 6));
  range.setAttribute(QStringLiteral("top"), QStringLiteral("5.000000"));
  range.setAttribute(QStringLiteral("bottom"), QStringLiteral("-5.000000"));
  plot_element.appendChild(range);
  QDomElement curve = doc.createElement(QStringLiteral("curve"));
  curve.setAttribute(QStringLiteral("name"), fixture.key);
  plot_element.appendChild(curve);

  ASSERT_TRUE(fixture.plot.xmlLoadState(plot_element, /*autozoom=*/true));

  const QRectF rect = fixture.plot.currentBoundingRect();
  EXPECT_NEAR(rect.left(), 0.0, 1e-3);
  EXPECT_NEAR(rect.right(), 1.0, 1e-3);
}

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
