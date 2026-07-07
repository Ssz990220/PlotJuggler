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
#include <QtGlobal>
#include <string>
#include <vector>

#include "pj_base/dataset.hpp"
#include "pj_base/type_tree.hpp"
#include "pj_datastore/writer.hpp"
#include "pj_plotting/PlotWidget.h"
#include "pj_plotting/SnapshotSeriesData.h"
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
