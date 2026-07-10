// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <string>
#include <string_view>
#include <vector>

#include "LayoutXml.h"
#include "MainWindow.h"
#include "SourceTimelineController.h"
#include "dataset_test_helpers.h"
#include "pj_runtime/AppSession.h"
#include "pj_runtime/CatalogModel.h"
#include "pj_runtime/SessionManager.h"
#include "pj_runtime/Time.h"

namespace PJ {

// Reaches MainWindow's private data-source save/apply seam plus the Source
// Timeline controller, so the round-trip runs against the real widget tree.
class MainWindowSourceLayoutTestPeer {
 public:
  [[nodiscard]] static AppSession& session(MainWindow& window) {
    return *window.session_;
  }

  [[nodiscard]] static QDomElement appendSources(MainWindow& window, QDomDocument& doc, const QDir& layout_dir) {
    return window.appendDataSourceElement(doc, layout_dir);
  }

  static void applyTimeline(MainWindow& window, const QList<layout_xml::DataSourceRef>& sources) {
    window.applyTimelineStateFromLayout(sources);
  }

  static void setTrackOrder(MainWindow& window, std::vector<DatasetId> order) {
    ASSERT_NE(window.source_timeline_controller_, nullptr);
    window.source_timeline_controller_->setDisplayOrder(std::move(order));
  }

  [[nodiscard]] static std::vector<DatasetId> trackOrder(const MainWindow& window) {
    EXPECT_NE(window.source_timeline_controller_, nullptr);
    return window.source_timeline_controller_ != nullptr ? window.source_timeline_controller_->currentTrackOrder()
                                                         : std::vector<DatasetId>{};
  }
};

}  // namespace PJ

namespace {

// Creates a dataset on its own TimeDomain (required for a per-source offset) and
// writes a two-sample topic, mirroring FileLoader's one-domain-per-source.
PJ::DatasetId addDataset(PJ::AppSession& app, std::string_view source_name, std::string_view topic) {
  const PJ::DatasetId dataset = pj_test::createDataset(app, source_name, /*own_time_domain=*/true);
  if (dataset == 0 || pj_test::addScalarTopic(app, dataset, topic, /*first_ts=*/1'000, /*second_ts=*/2'000) == 0) {
    return 0;
  }
  return dataset;
}

// One file fans out into two datasets; each carries its own alignment offset and
// timeline slot. The v4 <dataset> children must serialize every track and the
// apply path must restore each on the matching fan-out sibling — never swapping.
TEST(MainWindowSourceLayoutTest, OneFanoutFileRoundTripsEveryOffsetAndTrackOrder) {
  QTemporaryDir extensions_dir;
  QTemporaryDir project_dir;
  ASSERT_TRUE(extensions_dir.isValid());
  ASSERT_TRUE(project_dir.isValid());
  const QString source_path = project_dir.filePath(QStringLiteral("data/run.mcap"));
  ASSERT_TRUE(QDir().mkpath(QFileInfo(source_path).absolutePath()));
  QFile source_file(source_path);
  ASSERT_TRUE(source_file.open(QIODevice::WriteOnly));
  source_file.close();

  PJ::MainWindow window(extensions_dir.path());
  PJ::AppSession& app = PJ::MainWindowSourceLayoutTestPeer::session(window);
  PJ::SessionManager& session = app.sessionManager();
  const PJ::DatasetId left = addDataset(app, "run/left", "/left");
  const PJ::DatasetId right = addDataset(app, "run/right", "/right");
  ASSERT_NE(left, 0U);
  ASSERT_NE(right, 0U);
  app.catalogModel().rebuildFromDatastore();
  session.setDatasetSourcePath(left, source_path);
  session.setDatasetSourcePath(right, source_path);
  session.recordLoadedSource(source_path, QString{}, QStringLiteral("MCAP"), QStringLiteral(R"({"__pj_fanout":[]})"));

  session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{11'000}});
  session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{-22'000}});
  PJ::MainWindowSourceLayoutTestPeer::setTrackOrder(window, {right, left});

  QDomDocument doc;
  QDomElement root = doc.createElement(QStringLiteral("root"));
  root.setAttribute(QStringLiteral("pj4_version"), QStringLiteral("4"));
  doc.appendChild(root);
  const QDir layout_dir(project_dir.path());
  QDomElement wrapper = PJ::MainWindowSourceLayoutTestPeer::appendSources(window, doc, layout_dir);
  ASSERT_FALSE(wrapper.isNull());
  root.appendChild(wrapper);

  const QDomNodeList file_infos = wrapper.elementsByTagName(QStringLiteral("fileInfo"));
  ASSERT_EQ(file_infos.size(), 1) << "the source file must be replayed once, not once per fan-out dataset";
  const QDomElement file_info = file_infos.at(0).toElement();
  ASSERT_EQ(file_info.elementsByTagName(QStringLiteral("dataset")).size(), 2);

  const QList<PJ::layout_xml::DataSourceRef> refs = PJ::layout_xml::extractDataSource(doc, layout_dir);
  ASSERT_EQ(refs.size(), 1);
  ASSERT_EQ(refs.front().datasets.size(), 2);
  EXPECT_EQ(refs.front().datasets[0].source_name, QStringLiteral("run/left"));
  EXPECT_EQ(refs.front().datasets[0].source_index, 0);
  EXPECT_EQ(refs.front().datasets[0].display_offset_ns, 11'000);
  EXPECT_EQ(refs.front().datasets[0].timeline_order, 1);
  EXPECT_EQ(refs.front().datasets[1].source_name, QStringLiteral("run/right"));
  EXPECT_EQ(refs.front().datasets[1].source_index, 1);
  EXPECT_EQ(refs.front().datasets[1].display_offset_ns, -22'000);
  EXPECT_EQ(refs.front().datasets[1].timeline_order, 0);

  session.setDisplayOffset(left, PJ::DisplayOffset{PJ::Duration{0}});
  session.setDisplayOffset(right, PJ::DisplayOffset{PJ::Duration{0}});
  PJ::MainWindowSourceLayoutTestPeer::setTrackOrder(window, {left, right});
  PJ::MainWindowSourceLayoutTestPeer::applyTimeline(window, refs);

  EXPECT_EQ(session.sourceDisplayOffset(left).value.count(), 11'000);
  EXPECT_EQ(session.sourceDisplayOffset(right).value.count(), -22'000);
  EXPECT_EQ(PJ::MainWindowSourceLayoutTestPeer::trackOrder(window), (std::vector<PJ::DatasetId>{right, left}));
}

}  // namespace

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QStandardPaths::setTestModeEnabled(true);
  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
