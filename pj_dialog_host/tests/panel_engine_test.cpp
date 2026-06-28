// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <pj_plugins/host/dialog_handle.hpp>
#include <pj_plugins/host_qt/panel_engine.hpp>

#include "mock_panel_plugin.hpp"

// Defined in mock_panel_plugin.cpp via PJ_DIALOG_PLUGIN(MockPanelPlugin).
extern "C" const PJ_dialog_vtable_t* PJ_get_dialog_vtable() noexcept;

namespace {

// QApplication must exist before any QWidget is built. Created once per
// test executable and torn down at exit.
QApplication* qapp() {
  static int argc = 0;
  static QApplication app(argc, nullptr);
  // QSettings needs a non-empty organization/application identity to read and
  // write reliably across platforms: with an empty identity Windows returns
  // QSettings::AccessError, so writes are dropped and reads fall back to the
  // default. ThemeChangeReappliesWidgetData toggles the theme through QSettings,
  // so without this its toggle is a silent no-op on Windows (passes on Linux,
  // which is file-backed and lenient). Give the test process a stable identity.
  static const bool kIdentitySet = [] {
    QCoreApplication::setOrganizationName(QStringLiteral("PlotJugglerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("panel_engine_test"));
    return true;
  }();
  (void)kIdentitySet;
  return &app;
}

// Pump the event loop for `ms` milliseconds to let timers and queued
// connections fire.
void pumpEventLoop(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

PJ::DialogHandle makeMockHandle() {
  return PJ::DialogHandle(PJ_get_dialog_vtable());
}

}  // namespace

class PanelEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    qapp();
    resetMockPanelState();
  }
};

TEST_F(PanelEngineTest, OpenPanelReturnsConstructedWidget) {
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  // The mock UI has labelHello, textBox, and buttonClose.
  EXPECT_NE(panel->findChild<QLabel*>("labelHello"), nullptr);
  EXPECT_NE(panel->findChild<QLineEdit*>("textBox"), nullptr);
  EXPECT_NE(panel->findChild<QPushButton*>("buttonClose"), nullptr);

  delete panel;
}

TEST_F(PanelEngineTest, InitialWidgetDataIsApplied) {
  mockPanelState().label = "Hello from PanelEngine";
  mockPanelState().text = "preset";

  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* label = panel->findChild<QLabel*>("labelHello");
  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(label, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(label->text().toStdString(), "Hello from PanelEngine");
  EXPECT_EQ(text->text().toStdString(), "preset");

  delete panel;
}

TEST_F(PanelEngineTest, ThemeChangeReappliesWidgetData) {
  // Host-themed icons (setButtonIconNamed → loadSvg) are applied once and then
  // diffed away, so a live theme switch must re-apply the panel's last
  // widget-data to re-tint them. The icon color can't be observed here (no
  // resources linked), so we prove the re-apply fires by watching a plain widget
  // value get restored from the plugin's last data on a StyleChange.
  const QString saved_theme = QSettings().value(QStringLiteral("StyleSheet::theme")).toString();
  QSettings().setValue(QStringLiteral("StyleSheet::theme"), QStringLiteral("light"));

  mockPanelState().label = "FromPlugin";
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);
  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  ASSERT_EQ(label->text(), QStringLiteral("FromPlugin"));

  // Externally clobber the label. A normal tick won't restore it — the plugin's
  // data is unchanged, so the diff is empty — only the theme-change re-apply will.
  label->setText(QStringLiteral("CLOBBERED"));

  // Theme changes, then the app re-polishes the panel (StyleChange): the engine
  // re-applies the plugin's last data, restoring the label.
  QSettings().setValue(QStringLiteral("StyleSheet::theme"), QStringLiteral("dark"));
  QEvent style_change(QEvent::StyleChange);
  QApplication::sendEvent(panel, &style_change);
  EXPECT_EQ(label->text(), QStringLiteral("FromPlugin"));

  // A second StyleChange with no further theme change must NOT re-apply (the
  // applied-theme gate prevents redundant re-applies on unrelated polish events).
  label->setText(QStringLiteral("CLOBBERED2"));
  QEvent style_change2(QEvent::StyleChange);
  QApplication::sendEvent(panel, &style_change2);
  EXPECT_EQ(label->text(), QStringLiteral("CLOBBERED2"));

  if (saved_theme.isEmpty()) {
    QSettings().remove(QStringLiteral("StyleSheet::theme"));
  } else {
    QSettings().setValue(QStringLiteral("StyleSheet::theme"), saved_theme);
  }
  delete panel;
}

TEST_F(PanelEngineTest, TickPropagatesPluginStateChanges) {
  PJ::PanelEngine engine(
      makeMockHandle(), {/*tick_interval_ms=*/10, /*enable_diff=*/true, /*catalog_key_resolver=*/{}});
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  // After mutating plugin state, the next tick should refresh the label.
  mockPanelState().label = "Updated";
  pumpEventLoop(50);  // ample for several ticks

  auto* label = panel->findChild<QLabel*>("labelHello");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text().toStdString(), "Updated");
  EXPECT_GT(engine.stats().tick_count, 0);

  delete panel;
}

TEST_F(PanelEngineTest, RequestCloseFiresCallback) {
  PJ::PanelEngine engine(
      makeMockHandle(), {/*tick_interval_ms=*/10, /*enable_diff=*/true, /*catalog_key_resolver=*/{}});
  std::string captured_reason;
  bool fired = false;
  engine.onCloseRequested([&](std::string reason) {
    captured_reason = std::move(reason);
    fired = true;
  });

  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  // Ask the plugin to request close on next tick.
  mockPanelState().close_on_next_tick = true;
  mockPanelState().close_reason = "import_complete";

  pumpEventLoop(50);

  EXPECT_TRUE(fired);
  EXPECT_EQ(captured_reason, "import_complete");

  delete panel;
}

TEST_F(PanelEngineTest, ButtonBoxRejectClosesPanel) {
  // The panel's standard QDialogButtonBox (Close) is wired to the close path:
  // connectWidgetSignals skips button-box buttons and the non-modal panel has
  // no QDialog::reject, so without that wiring Close would be inert.
  PJ::PanelEngine engine(makeMockHandle());
  std::string captured_reason;
  bool fired = false;
  engine.onCloseRequested([&](std::string reason) {
    captured_reason = std::move(reason);
    fired = true;
  });

  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* button_box = panel->findChild<QDialogButtonBox*>("buttonBox");
  ASSERT_NE(button_box, nullptr);
  auto* close_btn = button_box->button(QDialogButtonBox::Close);
  ASSERT_NE(close_btn, nullptr);
  close_btn->click();

  pumpEventLoop(10);

  EXPECT_TRUE(fired);
  EXPECT_EQ(captured_reason, "closed by user");

  delete panel;
}

TEST_F(PanelEngineTest, WidgetEventReachesPlugin) {
  PJ::PanelEngine engine(
      makeMockHandle(), {/*tick_interval_ms=*/10, /*enable_diff=*/true, /*catalog_key_resolver=*/{}});
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  auto* text = panel->findChild<QLineEdit*>("textBox");
  ASSERT_NE(text, nullptr);

  text->setText("typed by test");
  // QLineEdit::textChanged is synchronous; let one tick run to apply diffed
  // state from the plugin back to the widget.
  pumpEventLoop(30);

  EXPECT_EQ(mockPanelState().text, "typed by test");
  EXPECT_GT(engine.stats().event_count, 0);

  delete panel;
}

TEST_F(PanelEngineTest, CloseIsIdempotent) {
  PJ::PanelEngine engine(makeMockHandle());
  QWidget* panel = engine.openPanel();
  ASSERT_NE(panel, nullptr);

  engine.close();
  engine.close();  // Must not crash.

  delete panel;
}
