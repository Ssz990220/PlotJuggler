// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// End-to-end harness for snapshot mode on a real (offscreen) PlotWidget: build a
// spline-shaped topic, add a snapshot curve group, and verify the curves' point
// counts and values track the message under the tracker as it moves — including a
// ragged message that must shrink the curves without leaking a prior message.

#include <gtest/gtest.h>
#include <qwt_plot_curve.h>
#include <qwt_series_data.h>

#include <QApplication>
#include <QDomDocument>
#include <QDomElement>
#include <QPen>
#include <QRectF>
#include <QtGlobal>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/query.hpp"
#include "pj_datastore/reader.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/SnapshotGroupDialog.h"
#include "pj_plotting/SnapshotSeriesData.h"
#include "pj_plotting/YAxisRangeDialog.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"

namespace PJ {
namespace {

constexpr Timestamp kNs = 1000000000;
constexpr int kElements = 5;
constexpr int kPositions = 2;

std::string posPath(int i, int j) {
  return "predicted_trajectory[" + std::to_string(i) + "].positions[" + std::to_string(j) + "]";
}
std::string stampPath(int i) {
  return "predicted_trajectory[" + std::to_string(i) + "].time_from_start_s";
}
double posValue(int m, int i, int j) {
  return 1000.0 * m + 10.0 * i + j;
}
double stampValue(int i) {
  return 0.5 * i;
}

// Returns the SnapshotSeriesData behind a plotted curve (or null).
const SnapshotSeriesData* snapshotOf(const PlotWidgetBase::CurveInfo& info) {
  return info.curve != nullptr ? dynamic_cast<const SnapshotSeriesData*>(info.curve->data()) : nullptr;
}

struct Fixture {
  SessionManager session;
  CatalogModel catalog{&session};
  DatasetId dataset_id = 0;
  TopicId topic_id = 0;
  std::vector<std::pair<std::string, std::size_t>> columns;  // (field_path, column index)

  Fixture() {
    dataset_id = *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "spline"});

    DataWriter writer = session.dataEngine().createWriter();
    auto positions = makeArray("positions", makePrimitive("", PrimitiveType::kFloat64), kPositions);
    auto element = makeStruct("", {positions, makePrimitive("time_from_start_s", PrimitiveType::kFloat64)});
    auto traj = makeArray("predicted_trajectory", element, kElements);
    auto root = makeStruct("SplineInfo", {traj});
    topic_id = *writer.registerTopic(
        dataset_id, TopicDescriptor{.name = "/spline", .schema_id = *writer.registerSchema("spline", root)});
    EXPECT_TRUE(writer.bindTopicWriter(topic_id).has_value());

    for (int i = 0; i < kElements; ++i) {
      for (int j = 0; j < kPositions; ++j) {
        columns.emplace_back(posPath(i, j), static_cast<std::size_t>(*writer.resolveField(topic_id, posPath(i, j))));
      }
      columns.emplace_back(stampPath(i), static_cast<std::size_t>(*writer.resolveField(topic_id, stampPath(i))));
    }

    writeMessage(writer, 0 * kNs, /*m=*/0, /*present=*/kElements);
    writeMessage(writer, 1 * kNs, /*m=*/1, /*present=*/3);  // ragged
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    catalog.rebuildFromDatastore();
  }

  std::size_t colOf(const std::string& path) const {
    for (const auto& [p, c] : columns) {
      if (p == path) {
        return c;
      }
    }
    return 0;
  }

  void writeMessage(DataWriter& writer, Timestamp t, int m, int present) {
    EXPECT_TRUE(writer.beginRow(topic_id, t).has_value());
    for (int i = 0; i < present; ++i) {
      for (int j = 0; j < kPositions; ++j) {
        writer.set(topic_id, colOf(posPath(i, j)), posValue(m, i, j));
      }
      writer.set(topic_id, colOf(stampPath(i)), stampValue(i));
    }
    EXPECT_TRUE(writer.finishRow(topic_id).has_value());
  }
};

TEST(PlotWidgetSnapshot, GroupAddsCurvesAndTracksTheMessageUnderTheTracker) {
  Fixture fx;
  PlotWidget plot(&fx.session, &fx.catalog);

  const auto added = plot.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]"),
       QStringLiteral("predicted_trajectory[:].positions[1]")});

  ASSERT_EQ(added.size(), 2U);
  EXPECT_TRUE(plot.isSnapshotPlot());
  ASSERT_EQ(plot.curveList().size(), 2U);

  // Tracker on the first (full) message: each curve carries all 5 elements.
  plot.setTrackerPosition(0.5);
  for (const auto& info : plot.curveList()) {
    const SnapshotSeriesData* series = snapshotOf(info);
    ASSERT_NE(series, nullptr);
    EXPECT_EQ(series->size(), static_cast<std::size_t>(kElements));
  }

  // Verify the positions[1] curve's actual points against message m=0.
  const SnapshotSeriesData* pos1 = snapshotOf(plot.curveList().back());
  ASSERT_NE(pos1, nullptr);
  for (std::size_t i = 0; i < static_cast<std::size_t>(kElements); ++i) {
    EXPECT_DOUBLE_EQ(pos1->sample(i).x(), stampValue(static_cast<int>(i)));
    EXPECT_DOUBLE_EQ(pos1->sample(i).y(), posValue(0, static_cast<int>(i), 1));
  }
}

TEST(PlotWidgetSnapshot, RaggedMessageShrinksCurvesWithoutLeakingPriorMessage) {
  Fixture fx;
  PlotWidget plot(&fx.session, &fx.catalog);
  plot.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]")});

  plot.setTrackerPosition(0.5);  // full message: 5 elements
  ASSERT_EQ(snapshotOf(plot.curveList().front())->size(), static_cast<std::size_t>(kElements));

  plot.setTrackerPosition(1.5);  // ragged message m=1: only 3 elements
  const SnapshotSeriesData* series = snapshotOf(plot.curveList().front());
  ASSERT_NE(series, nullptr);
  ASSERT_EQ(series->size(), 3U);
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(series->sample(i).y(), posValue(1, static_cast<int>(i), 0));
  }
}

TEST(PlotWidgetSnapshot, IndexModeWhenNoXPattern) {
  Fixture fx;
  PlotWidget plot(&fx.session, &fx.catalog);
  const auto added = plot.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QString(), {QStringLiteral("predicted_trajectory[:].positions[0]")});
  ASSERT_EQ(added.size(), 1U);

  plot.setTrackerPosition(0.5);
  const SnapshotSeriesData* series = snapshotOf(plot.curveList().front());
  ASSERT_NE(series, nullptr);
  ASSERT_EQ(series->size(), static_cast<std::size_t>(kElements));
  for (std::size_t i = 0; i < static_cast<std::size_t>(kElements); ++i) {
    EXPECT_DOUBLE_EQ(series->sample(i).x(), static_cast<double>(i));  // X = element index
  }
}

TEST(PlotWidgetSnapshot, XmlRoundTripReproducesRender) {
  Fixture fx;
  PlotWidget src(&fx.session, &fx.catalog);
  src.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]"),
       QStringLiteral("predicted_trajectory[:].positions[1]")});
  src.setTrackerPosition(0.5);

  QDomDocument doc;
  QDomElement elem = src.xmlSaveState(doc);
  doc.appendChild(elem);
  EXPECT_EQ(elem.attribute(QStringLiteral("mode")), QStringLiteral("Snapshot"));

  // Reload into a FRESH widget sharing the same session/catalog: the snapshot group
  // re-resolves from the persisted topic + patterns (no app-level rebind needed).
  PlotWidget dst(&fx.session, &fx.catalog);
  ASSERT_TRUE(dst.xmlLoadState(elem));
  EXPECT_TRUE(dst.isSnapshotPlot());
  ASSERT_EQ(dst.curveList().size(), 2U);

  src.setTrackerPosition(0.5);
  dst.setTrackerPosition(0.5);

  // Every source curve has an identically-keyed reloaded curve with the same color
  // and the same points at the cursor time.
  for (const auto& src_info : src.curveList()) {
    const SnapshotSeriesData* s = snapshotOf(src_info);
    ASSERT_NE(s, nullptr);
    PlotWidgetBase::CurveInfo* dst_info = dst.curveFromTitle(src_info.source_name);
    ASSERT_NE(dst_info, nullptr) << src_info.source_name.toStdString();
    const SnapshotSeriesData* d = snapshotOf(*dst_info);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(dst_info->curve->pen().color(), src_info.curve->pen().color());
    ASSERT_EQ(d->size(), s->size());
    for (std::size_t k = 0; k < s->size(); ++k) {
      EXPECT_DOUBLE_EQ(d->sample(k).x(), s->sample(k).x());
      EXPECT_DOUBLE_EQ(d->sample(k).y(), s->sample(k).y());
    }
  }
}

TEST(PlotWidgetSnapshot, XmlLoadWithMissingTopicDegradesGracefully) {
  Fixture fx;
  PlotWidget src(&fx.session, &fx.catalog);
  src.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]")});
  QDomDocument doc;
  QDomElement elem = src.xmlSaveState(doc);
  doc.appendChild(elem);

  // Load into a widget whose session/catalog do NOT contain the topic. The load
  // succeeds, snapshot mode is restored, and the unresolvable curve simply never
  // appears — same graceful degradation as a missing regular curve.
  PJ::SessionManager empty_session;
  PJ::CatalogModel empty_catalog(&empty_session);
  PlotWidget dst(&empty_session, &empty_catalog);
  EXPECT_TRUE(dst.xmlLoadState(elem));
  EXPECT_TRUE(dst.isSnapshotPlot());
  EXPECT_TRUE(dst.curveList().empty());
}

TEST(PlotWidgetSnapshot, UnresolvableGroupIsNoOp) {
  Fixture fx;
  PlotWidget plot(&fx.session, &fx.catalog);
  const auto added = plot.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QString(), {QStringLiteral("nonexistent[:].field")});
  EXPECT_TRUE(added.empty());
  EXPECT_FALSE(plot.isSnapshotPlot());
  EXPECT_TRUE(plot.curveList().empty());
}

// A datastore whose messages sit at ABSOLUTE epoch-ns timestamps (like a real
// mcap), the first message well AFTER display-time 0. Restoring a snapshot plot at
// the default seed (time 0) therefore sees no data — the exact in-app condition
// under which the plot used to pin an empty axis and never update.
constexpr Timestamp kAbsBaseNs = 1783389169000000000LL;  // ~2026-07-06 in ns
constexpr Timestamp kStepNs = 100000000LL;               // 100 ms between messages
constexpr int kAbsMessages = 6;

struct AbsFixture {
  SessionManager session;
  CatalogModel catalog{&session};
  DatasetId dataset_id = 0;
  TopicId topic_id = 0;
  std::vector<std::pair<std::string, std::size_t>> columns;

  AbsFixture() {
    dataset_id = *session.dataEngine().createDataset(DatasetDescriptor{.source_name = "spline"});
    DataWriter writer = session.dataEngine().createWriter();
    auto positions = makeArray("positions", makePrimitive("", PrimitiveType::kFloat64), kPositions);
    auto element = makeStruct("", {positions, makePrimitive("time_from_start_s", PrimitiveType::kFloat64)});
    auto traj = makeArray("predicted_trajectory", element, kElements);
    auto root = makeStruct("SplineInfo", {traj});
    topic_id = *writer.registerTopic(
        dataset_id, TopicDescriptor{.name = "/spline", .schema_id = *writer.registerSchema("spline", root)});
    EXPECT_TRUE(writer.bindTopicWriter(topic_id).has_value());
    for (int i = 0; i < kElements; ++i) {
      for (int j = 0; j < kPositions; ++j) {
        columns.emplace_back(posPath(i, j), static_cast<std::size_t>(*writer.resolveField(topic_id, posPath(i, j))));
      }
      columns.emplace_back(stampPath(i), static_cast<std::size_t>(*writer.resolveField(topic_id, stampPath(i))));
    }
    for (int k = 0; k < kAbsMessages; ++k) {
      EXPECT_TRUE(writer.beginRow(topic_id, kAbsBaseNs + k * kStepNs).has_value());
      for (int i = 0; i < kElements; ++i) {
        for (int j = 0; j < kPositions; ++j) {
          writer.set(topic_id, colOf(posPath(i, j)), posValue(k, i, j));
        }
        writer.set(topic_id, colOf(stampPath(i)), stampValue(i));
      }
      EXPECT_TRUE(writer.finishRow(topic_id).has_value());
    }
    EXPECT_FALSE(session.commitChunks(writer.flushAll()).empty());
    catalog.rebuildFromDatastore();
  }
  std::size_t colOf(const std::string& path) const {
    for (const auto& [p, c] : columns) {
      if (p == path) {
        return c;
      }
    }
    return 0;
  }
  // Display-axis seconds for message k (offset is 0, so display == raw/1e9), nudged
  // +10 ms so the display->raw round-trip lands at-or-after the message despite
  // double rounding at ~1.78e9 s.
  double displayTimeForMessage(int k) const {
    return static_cast<double>(kAbsBaseNs + k * kStepNs + 10000000LL) / 1e9;
  }
};

// The core regression: restore a snapshot plot BEFORE the topic's first message,
// then step the tracker across real data times. The points must change, match
// latestRowAt ground truth, AND the axis must frame the data (the old code pinned a
// degenerate axis fit to the empty restore state, so the live points were invisible).
TEST(PlotWidgetSnapshot, RestoredBeforeFirstMessageThenScrubsUpdatesAndFits) {
  AbsFixture fx;

  // Save from a source plot (its own seed is time 0 -> empty snapshot at save)...
  PlotWidget src(&fx.session, &fx.catalog);
  src.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]")});
  QDomDocument doc;
  QDomElement elem = src.xmlSaveState(doc);
  doc.appendChild(elem);

  // ...restore into a fresh plot. Seed is time 0, before the first message: empty.
  PlotWidget dst(&fx.session, &fx.catalog);
  ASSERT_TRUE(dst.xmlLoadState(elem));
  ASSERT_EQ(dst.curveList().size(), 1U);
  const SnapshotSeriesData* series = snapshotOf(dst.curveList().front());
  ASSERT_NE(series, nullptr);
  EXPECT_EQ(series->size(), 0U) << "restored snapshot should be empty before the first message";

  DataReader reader = fx.session.createReader();
  const std::size_t y_col = fx.colOf(posPath(0, 0));

  double prev_first_y = std::numeric_limits<double>::quiet_NaN();
  for (int k = 0; k < kAbsMessages; ++k) {
    dst.setTrackerPosition(fx.displayTimeForMessage(k));

    // (1) Points appear and match the message under the tracker.
    ASSERT_EQ(series->size(), static_cast<std::size_t>(kElements)) << "k=" << k;
    // Ground truth straight from the datastore at the same raw time.
    const auto row = reader.latestRowAt(
        QueryPoint{.topic_id = fx.topic_id, .t = kAbsBaseNs + k * kStepNs + 10000000LL}, {y_col});
    ASSERT_TRUE(row.has_value() && row->has_value() && (*row)->values[0].has_value()) << "k=" << k;
    EXPECT_DOUBLE_EQ(series->sample(0).y(), *(*row)->values[0]) << "k=" << k;
    EXPECT_DOUBLE_EQ(series->sample(0).y(), posValue(k, 0, 0)) << "k=" << k;

    // (2) The point set CHANGES as the cursor advances between messages.
    if (k > 0) {
      EXPECT_NE(series->sample(0).y(), prev_first_y) << "snapshot did not update at k=" << k;
    }
    prev_first_y = series->sample(0).y();

    // (3) REGRESSION: the axis frames the data — non-degenerate and containing the
    // points. The pre-fix code left this a degenerate rect (fit to the empty restore
    // state), so the real points rendered off-screen ("doesn't update").
    const QRectF view = dst.currentBoundingRect();
    EXPECT_GT(view.width(), 0.1) << "k=" << k << " degenerate X axis (fit-to-empty regression)";
    EXPECT_GT(std::abs(view.height()), 1e-9) << "k=" << k << " degenerate Y axis";
    const QPointF p0 = series->sample(0);
    EXPECT_GE(p0.x(), std::min(view.left(), view.right()) - 1e-6) << "k=" << k;
    EXPECT_LE(p0.x(), std::max(view.left(), view.right()) + 1e-6) << "k=" << k;
  }
}

// Y-axis extent of the plot's current viewport as [min, max].
std::pair<double, double> viewY(const PlotWidget& plot) {
  const QRectF v = plot.currentBoundingRect();
  return {std::min(v.top(), v.bottom()), std::max(v.top(), v.bottom())};
}

// A manual full y-range must survive save/reload, be valid on an EMPTY restore
// (before the topic's first message), and stay pinned across playback while X still
// fits each message — i.e. playback never stomps Y.
TEST(PlotWidgetSnapshot, FixedYRangePinnedAcrossRestoreEmptyAndPlayback) {
  AbsFixture fx;
  PlotWidget src(&fx.session, &fx.catalog);
  src.addSnapshotCurveGroup(
      fx.dataset_id, fx.topic_id, QStringLiteral("predicted_trajectory[:].time_from_start_s"),
      {QStringLiteral("predicted_trajectory[:].positions[0]")});
  src.setFixedYRange(-5.0, 5.0);

  QDomDocument doc;
  QDomElement elem = src.xmlSaveState(doc);
  doc.appendChild(elem);
  EXPECT_DOUBLE_EQ(elem.attribute(QStringLiteral("fixed_y_min")).toDouble(), -5.0);
  EXPECT_DOUBLE_EQ(elem.attribute(QStringLiteral("fixed_y_max")).toDouble(), 5.0);

  // Reload into a fresh plot; the tracker seed is time 0 (before the first message).
  PlotWidget dst(&fx.session, &fx.catalog);
  ASSERT_TRUE(dst.xmlLoadState(elem));
  ASSERT_TRUE(dst.fixedYMin().has_value() && dst.fixedYMax().has_value());
  EXPECT_DOUBLE_EQ(*dst.fixedYMin(), -5.0);
  EXPECT_DOUBLE_EQ(*dst.fixedYMax(), 5.0);

  // Empty restore: the Y axis is already the pinned range (no fit-to-empty on Y).
  auto [y0lo, y0hi] = viewY(dst);
  EXPECT_NEAR(y0lo, -5.0, 1e-6);
  EXPECT_NEAR(y0hi, 5.0, 1e-6);

  const SnapshotSeriesData* series = snapshotOf(dst.curveList().front());
  ASSERT_NE(series, nullptr);

  for (int k = 0; k < kAbsMessages; ++k) {
    dst.setTrackerPosition(fx.displayTimeForMessage(k));
    // Points update (the message under the tracker), even though its values (posValue
    // grows into the thousands) are far outside the pinned [-5, 5] view.
    ASSERT_EQ(series->size(), static_cast<std::size_t>(kElements)) << "k=" << k;
    EXPECT_DOUBLE_EQ(series->sample(0).y(), posValue(k, 0, 0)) << "k=" << k;

    // Y stays pinned to [-5, 5]; playback never stomps it.
    auto [ylo, yhi] = viewY(dst);
    EXPECT_NEAR(ylo, -5.0, 1e-6) << "k=" << k;
    EXPECT_NEAR(yhi, 5.0, 1e-6) << "k=" << k;

    // X still fits the current message (stamps span [0, 0.5*(kElements-1)]).
    const QRectF v = dst.currentBoundingRect();
    EXPECT_NEAR(std::min(v.left(), v.right()), 0.0, 1e-6) << "k=" << k;
    EXPECT_NEAR(std::max(v.left(), v.right()), stampValue(kElements - 1), 1e-6) << "k=" << k;
  }
}

// A half-open pin (fixed max, auto min) pins only the pinned bound and auto-fits the
// other. Uses a datastore curve as a regular time-series plot to keep the data range
// stable and on the auto side of the pin.
TEST(PlotWidgetSnapshot, HalfOpenFixedMaxPinsMaxAutoFitsMin) {
  AbsFixture fx;  // predicted_trajectory[0]/positions[0] over time = posValue(k,0,0) = 1000*k in [0, 5000]
  PlotWidget plot(&fx.session, &fx.catalog);
  QString key;
  for (const auto& curve : fx.catalog.curves()) {
    if (const auto d = fx.catalog.curveDescriptor(curve.name);
        d && d->topic_id == fx.topic_id && d->field_path == QString::fromStdString(posPath(0, 0))) {
      key = curve.name;
      break;
    }
  }
  ASSERT_FALSE(key.isEmpty());
  ASSERT_NE(plot.addCurve(key), nullptr);

  plot.setFixedYRange(std::nullopt, 9000.0);  // max pinned above the data, min auto
  plot.zoomOut(false);
  auto [ylo, yhi] = viewY(plot);
  EXPECT_NEAR(yhi, 9000.0, 1e-6) << "max must be pinned";
  EXPECT_LT(ylo, 9000.0) << "min must auto-fit (not pinned)";
  EXPECT_LE(ylo, 0.0 + 1e-6) << "min auto-fits the data floor (~0)";
}

// A regular TimeSeries plot honors a persisted fixed y-range too (the user asked for
// "each plot"), and clearing the pins returns it to auto-fit.
TEST(PlotWidgetSnapshot, TimeSeriesPlotHonorsFixedYAndClears) {
  AbsFixture fx;
  PlotWidget plot(&fx.session, &fx.catalog);
  QString key;
  for (const auto& curve : fx.catalog.curves()) {
    if (const auto d = fx.catalog.curveDescriptor(curve.name);
        d && d->topic_id == fx.topic_id && d->field_path == QString::fromStdString(posPath(0, 0))) {
      key = curve.name;
      break;
    }
  }
  ASSERT_FALSE(key.isEmpty());
  ASSERT_NE(plot.addCurve(key), nullptr);
  EXPECT_FALSE(plot.isSnapshotPlot());

  plot.setFixedYRange(-1.0, 1.0);
  plot.zoomOut(false);
  auto [ylo, yhi] = viewY(plot);
  EXPECT_NEAR(ylo, -1.0, 1e-6);
  EXPECT_NEAR(yhi, 1.0, 1e-6);

  // Clearing the pins restores auto-fit (data floor ~0, so min <= 0 < 1).
  plot.setFixedYRange(std::nullopt, std::nullopt);
  plot.zoomOut(false);
  auto [c_lo, c_hi] = viewY(plot);
  EXPECT_GT(c_hi, 1.0) << "auto-fit should exceed the old pinned max";
  EXPECT_FALSE(plot.hasFixedYRange());
}

// The Y Axis Range dialog seeds from the current pins and reports each bound
// (nullopt for a bound left on Auto).
TEST(YAxisRangeDialogTest, SeedsAndReportsBounds) {
  YAxisRangeDialog half(std::nullopt, 5.0);
  EXPECT_FALSE(half.yMin().has_value());
  ASSERT_TRUE(half.yMax().has_value());
  EXPECT_DOUBLE_EQ(*half.yMax(), 5.0);

  YAxisRangeDialog full(-2.0, 2.0);
  ASSERT_TRUE(full.yMin().has_value() && full.yMax().has_value());
  EXPECT_DOUBLE_EQ(*full.yMin(), -2.0);
  EXPECT_DOUBLE_EQ(*full.yMax(), 2.0);
}

// The snapshot-group dialog populates its topic combo from the catalog and defaults
// to index x-mode with nothing selected.
TEST(SnapshotGroupDialogTest, PopulatesTopicAndDefaults) {
  AbsFixture fx;
  SnapshotGroupDialog dialog(&fx.catalog);
  EXPECT_EQ(dialog.topicId(), fx.topic_id);
  EXPECT_EQ(dialog.datasetId(), fx.dataset_id);
  EXPECT_TRUE(dialog.xPattern().isEmpty());  // index x-mode by default
  EXPECT_TRUE(dialog.yPatterns().isEmpty());
  EXPECT_FALSE(dialog.yMin().has_value());
  EXPECT_FALSE(dialog.yMax().has_value());
}

}  // namespace
}  // namespace PJ

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
