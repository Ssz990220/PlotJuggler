// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <vector>

#include "pj_runtime/QSettingsBackend.h"

namespace {

TEST(QSettingsBackend, AbsentKeyReportsNulloptAndNotContained) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  EXPECT_FALSE(backend.getString("mosaico/metadata_query").has_value());
  EXPECT_FALSE(backend.getStringList("mosaico/server_history").has_value());
  EXPECT_FALSE(backend.contains("mosaico/metadata_query"));
}

TEST(QSettingsBackend, RoundTripsString) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  backend.setString("mosaico/metadata_query", "topic ~ 'cam'");
  const auto value = backend.getString("mosaico/metadata_query");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "topic ~ 'cam'");
}

TEST(QSettingsBackend, RoundTripsStringList) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  const std::vector<std::string> servers{"grpc+tls://a:1", "grpc+tls://b:2"};
  backend.setStringList("mosaico/server_history", servers);
  const auto got = backend.getStringList("mosaico/server_history");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, servers);
}

TEST(QSettingsBackend, ContainsThenRemove) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  PJ::QSettingsBackend backend(dir.filePath("settings.ini"));

  backend.setString("mosaico/range_lower", "42");
  EXPECT_TRUE(backend.contains("mosaico/range_lower"));

  backend.remove("mosaico/range_lower");
  EXPECT_FALSE(backend.contains("mosaico/range_lower"));
  EXPECT_FALSE(backend.getString("mosaico/range_lower").has_value());
}

TEST(QSettingsBackend, PersistsAcrossInstancesOnSameFile) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("settings.ini");

  {
    PJ::QSettingsBackend writer(path);
    writer.setString("mosaico/server_cache/h/api_key", "secret");
    writer.setStringList("mosaico/server_history", {"grpc+tls://a:1"});
  }

  PJ::QSettingsBackend reader(path);
  const auto key = reader.getString("mosaico/server_cache/h/api_key");
  ASSERT_TRUE(key.has_value());
  EXPECT_EQ(*key, "secret");

  const auto history = reader.getStringList("mosaico/server_history");
  ASSERT_TRUE(history.has_value());
  ASSERT_EQ(history->size(), 1U);
  EXPECT_EQ((*history)[0], "grpc+tls://a:1");
}

}  // namespace
