// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QFileDialog>
#include <QTimer>

#include "pj_widgets/FileDialog.h"

namespace {

class FileDialogTest : public ::testing::Test {};

// The dialog is modal — we close it from a single-shot timer so the test
// doesn't hang. We verify the embedded extras checkbox state is reported
// back to the caller, which is the whole reason the overload exists.
TEST_F(FileDialogTest, GetSaveFileNameWithOptionsReportsCheckboxState) {
  PJ::FileDialog::ExtraOption opt{QStringLiteral("Save Data Source"), /*default_checked=*/true};

  // Fire after the modal dialog is up; find the checkbox by its label and
  // toggle it off, then accept by hitting the Save button.
  QTimer::singleShot(0, [] {
    QWidget* active = QApplication::activeModalWidget();
    ASSERT_NE(active, nullptr);
    auto* checkbox = active->findChild<QCheckBox*>();
    ASSERT_NE(checkbox, nullptr);
    EXPECT_TRUE(checkbox->isChecked());
    checkbox->setChecked(false);
    auto* file_dialog = active->findChild<QFileDialog*>();
    ASSERT_NE(file_dialog, nullptr);
    file_dialog->selectFile(QStringLiteral("/tmp/_pj4_filedialog_test.pjl4"));
    QMetaObject::invokeMethod(file_dialog, "accept", Qt::QueuedConnection);
  });

  const auto result = PJ::FileDialog::getSaveFileNameWithOptions(
      /*parent=*/nullptr, QStringLiteral("Save"), QStringLiteral("/tmp"), QStringLiteral("PJ4 Layout (*.pjl4)"),
      QStringLiteral("pjl4"), {opt});

  EXPECT_FALSE(result.path.isEmpty());
  ASSERT_EQ(result.option_states.size(), 1u);
  EXPECT_FALSE(result.option_states[0]);  // toggled off in the timer
}

TEST_F(FileDialogTest, GetSaveFileNameWithOptionsCancelReturnsEmpty) {
  PJ::FileDialog::ExtraOption opt{QStringLiteral("Save Data Source"), true};
  QTimer::singleShot(0, [] {
    QWidget* active = QApplication::activeModalWidget();
    ASSERT_NE(active, nullptr);
    auto* file_dialog = active->findChild<QFileDialog*>();
    ASSERT_NE(file_dialog, nullptr);
    QMetaObject::invokeMethod(file_dialog, "reject", Qt::QueuedConnection);
  });

  const auto result = PJ::FileDialog::getSaveFileNameWithOptions(
      nullptr, QStringLiteral("Save"), QStringLiteral("/tmp"), QStringLiteral("PJ4 Layout (*.pjl4)"),
      QStringLiteral("pjl4"), {opt});

  EXPECT_TRUE(result.path.isEmpty());
  ASSERT_EQ(result.option_states.size(), 1u);
  EXPECT_TRUE(result.option_states[0]);  // defaults preserved on cancel
}

}  // namespace

int main(int argc, char** argv) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  testing::InitGoogleTest(&argc, argv);
  QApplication app(argc, argv);
  return RUN_ALL_TESTS();
}
