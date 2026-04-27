#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <vector>

#include "pj_app_core/AppSession.h"
#include "pj_app_core/ExtensionCatalogService.h"
#include "pj_marketplace/extension_manager.hpp"

namespace {

TEST(AppSessionTest, CustomExtensionDirectoryReachesMarketplaceManager) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());

  PJ::AppSession session(dir.path());

  EXPECT_EQ(session.extensionCatalog().extensionsDir(), dir.path());
  EXPECT_EQ(session.extensionCatalog().extensionManager().extensionsDir(), dir.path());
}

TEST(AppSessionTest, InvalidExtensionDirectoryReportsDiagnostic) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString file_path = dir.filePath("not-a-directory");
  QFile file(file_path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.close();

  std::vector<PJ::Diagnostic> diagnostics;
  PJ::AppSession session(
      file_path, [&diagnostics](const PJ::Diagnostic& diagnostic) { diagnostics.push_back(diagnostic); });

  const auto is_error = [](const PJ::Diagnostic& diagnostic) {
    return diagnostic.level == PJ::DiagnosticLevel::kError;
  };
  EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), is_error));
}

}  // namespace
