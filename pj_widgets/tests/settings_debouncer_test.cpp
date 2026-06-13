// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QTest>
#include <QVariant>

#include "pj_widgets/SettingsDebouncer.h"

namespace {

// Read back through a fresh default QSettings (same scope the debouncer writes
// to). Returns an invalid QVariant when the key is absent.
QVariant readBack(const QString& key) {
  return QSettings().value(key);
}

// queue() must NOT touch QSettings synchronously — that is the whole point
// (one INI rewrite per settle window, not per tick).
TEST(SettingsDebouncer, QueueDefersTheWriteUntilFlush) {
  QSettings().remove(QStringLiteral("test/deferred"));
  PJ::SettingsDebouncer debouncer;

  debouncer.queue(QStringLiteral("test/deferred"), 42);
  EXPECT_TRUE(debouncer.hasPending());
  EXPECT_FALSE(readBack(QStringLiteral("test/deferred")).isValid());  // not written yet

  debouncer.flush();
  EXPECT_FALSE(debouncer.hasPending());
  EXPECT_EQ(readBack(QStringLiteral("test/deferred")).toInt(), 42);
}

// Repeated queues for one key before a flush keep only the latest value, and a
// whole drag collapses to a single written value.
TEST(SettingsDebouncer, CoalescesRepeatedQueuesToLatestValue) {
  QSettings().remove(QStringLiteral("test/coalesce"));
  PJ::SettingsDebouncer debouncer;

  for (int tick = 0; tick <= 100; ++tick) {
    debouncer.queue(QStringLiteral("test/coalesce"), tick);
  }
  debouncer.flush();
  EXPECT_EQ(readBack(QStringLiteral("test/coalesce")).toInt(), 100);
}

// The bulk overload persists a whole group in one shot (reset-to-defaults shape).
TEST(SettingsDebouncer, BulkQueueWritesEveryKey) {
  QSettings settings;
  settings.remove(QStringLiteral("test/bulk_a"));
  settings.remove(QStringLiteral("test/bulk_b"));
  PJ::SettingsDebouncer debouncer;

  debouncer.queue(
      QHash<QString, QVariant>{
          {QStringLiteral("test/bulk_a"), 1},
          {QStringLiteral("test/bulk_b"), QStringLiteral("two")},
      });
  debouncer.flush();
  EXPECT_EQ(readBack(QStringLiteral("test/bulk_a")).toInt(), 1);
  EXPECT_EQ(readBack(QStringLiteral("test/bulk_b")).toString(), QStringLiteral("two"));
}

// The debounce timer flushes on its own after the settle window, with no
// explicit flush() call.
TEST(SettingsDebouncer, TimerFlushesAfterSettleWindow) {
  QSettings().remove(QStringLiteral("test/timed"));
  PJ::SettingsDebouncer debouncer(QString(), 20);  // short window for the test

  debouncer.queue(QStringLiteral("test/timed"), 7);
  EXPECT_FALSE(readBack(QStringLiteral("test/timed")).isValid());

  QTest::qWait(80);  // let the single-shot timer fire on the event loop
  EXPECT_FALSE(debouncer.hasPending());
  EXPECT_EQ(readBack(QStringLiteral("test/timed")).toInt(), 7);
}

// A non-empty group prefixes the keys (beginGroup), matching how callers like
// Scene3DConfigPanel scope their writes.
TEST(SettingsDebouncer, GroupPrefixesKeys) {
  QSettings().remove(QStringLiteral("test_group"));
  PJ::SettingsDebouncer debouncer(QStringLiteral("test_group"));

  debouncer.queue(QStringLiteral("nested"), QStringLiteral("v"));
  debouncer.flush();
  EXPECT_EQ(readBack(QStringLiteral("test_group/nested")).toString(), QStringLiteral("v"));
}

// Destruction flushes any pending writes (a panel closed mid-drag must not lose
// the last value).
TEST(SettingsDebouncer, DestructorFlushesPending) {
  QSettings().remove(QStringLiteral("test/dtor"));
  {
    PJ::SettingsDebouncer debouncer;
    debouncer.queue(QStringLiteral("test/dtor"), 99);
    EXPECT_FALSE(readBack(QStringLiteral("test/dtor")).isValid());
  }  // dtor runs flush()
  EXPECT_EQ(readBack(QStringLiteral("test/dtor")).toInt(), 99);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("PlotJugglerTest"));
  QCoreApplication::setApplicationName(QStringLiteral("SettingsDebouncerTest"));
  // Redirect QSettings to an isolated test location so the suite never touches
  // the developer's real PlotJuggler4.conf.
  QStandardPaths::setTestModeEnabled(true);
  QSettings().clear();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
